#pragma once

// Embedded one-page browser UI for the tuner controller (served by
// http_server.cpp at GET /). Polls /api/status once a second and drives
// the /api/* verbs. Cards are rendered per carrier axis, labelled with
// the element the topology binds to it; the header carries the network
// state (bypass, side, RF lockout, homed) and a topology editor.
//
// Dark, monospace, no external assets — the whole page is this literal.
// Keep it self-contained: the controller has no filesystem to serve from.

static const char INDEX_HTML[] =
R"HTML(<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Antenna Tuner Controller</title>
<style>
*{box-sizing:border-box}
body{font-family:ui-monospace,Menlo,monospace;background:#111;color:#eee;margin:0;padding:1em;line-height:1.4}
h1{margin:0 0 .25em;font-weight:300;font-size:1.4em}
h2{margin:0 0 .35em;font-weight:300;font-size:1.1em}
h3{margin:.2em 0 .4em;font-weight:300;font-size:1em;color:#aaa}
.top{color:#888;font-size:.85em;margin-bottom:.6em}
.top b{color:#eee}
.panel{border:1px solid #333;padding:.8em 1em;margin:.6em 0;border-radius:8px;background:#1a1a1a}
.axis.spare{opacity:.75}
.axis.atlimit{border-color:#f44}
.axis.unbound{border-style:dashed}
.row{display:flex;flex-wrap:wrap;gap:.4em;align-items:center;margin:.4em 0}
button{background:#2a2a2a;color:#eee;border:1px solid #555;padding:.45em .9em;border-radius:4px;cursor:pointer;font-family:inherit;font-size:.9em}
button:hover{background:#3a3a3a}
button.go{background:#1c3a1c;border-color:#494}
button.go:hover{background:#264826}
button.sel{background:#3a3a1c;border-color:#aa4}
button.big{font-size:1.05em;padding:.6em 1.2em}
code{color:#8bf}
input[type=number],select{background:#222;color:#eee;border:1px solid #555;padding:.4em;border-radius:4px;width:7em;font-size:1em;font-family:inherit}
select{width:auto}
.kv{font-size:.85em;color:#aaa}
.kv b{color:#eee}
.sep{color:#555;margin:0 .4em}
.bdg{display:none;color:#000;padding:.1em .55em;border-radius:.4em;font-size:.7em;margin-left:.5em;vertical-align:middle;font-weight:bold}
.bdg.on{display:inline-block}
.bdg.warn{background:#fc3}
.bdg.red{background:#f44;color:#fff;animation:pulse 1s ease-in-out infinite}
.bdg.run{background:#48c;color:#fff}
.bdg.dim{background:#555;color:#ddd}
.state{font-size:1.1em;padding:.35em .8em;border-radius:.4em;font-weight:bold;display:inline-block;margin-right:.6em}
.state.byp{background:#fc3;color:#000}
.state.eng{background:#4f4;color:#000}
.state.rf{background:#f44;color:#fff;animation:pulse 1s ease-in-out infinite}
/* Industrial E-stop: red mushroom head on a yellow collar */
button.estop{background:radial-gradient(circle at 35% 30%,#f66 0,#d00 45%,#800 100%);border:5px solid #fd0;border-radius:50%;width:70px;height:70px;color:#fff;font-weight:bold;font-size:.62em;line-height:1.1;padding:0;box-shadow:0 3px 0 #600,0 4px 6px rgba(0,0,0,.6);text-shadow:0 1px 1px #000}
button.estop:hover{background:radial-gradient(circle at 35% 30%,#f88 0,#e00 45%,#900 100%)}
button.estop:active{box-shadow:0 0 0 #600;transform:translateY(3px)}
button.estop.mid{width:66px;height:66px;font-size:.56em;border-width:5px;margin-left:.6em}
/* Pressed (latched) mushroom: sunk in, lit and strobing; click again to release */
button.estop.down{transform:translateY(3px);box-shadow:0 0 0 #600,0 0 22px 6px rgba(255,40,40,.8);border-color:#fff;animation:strobe .5s steps(2,end) infinite}
button.estop.down:active{transform:translateY(0)}
/* Beacon: rotating red lamp, dark when idle, strobing when the alarm is latched */
.beacon{display:inline-block;width:26px;height:26px;border-radius:50%;background:#3a0a0a;border:2px solid #555;vertical-align:middle;margin:0 .4em;position:relative}
.beacon.big{width:44px;height:44px}
.beacon.on{background:radial-gradient(circle at 40% 35%,#fff 0,#f44 30%,#c00 70%);border-color:#f88;animation:strobe .5s steps(2,end) infinite;box-shadow:0 0 18px 6px rgba(255,40,40,.75)}
.beacon.on::after{content:'';position:absolute;inset:-2px;border-radius:50%;background:conic-gradient(from 0deg,rgba(255,255,255,.55) 0 40deg,transparent 40deg 180deg,rgba(255,255,255,.35) 180deg 220deg,transparent 220deg 360deg);animation:sweep .9s linear infinite}
.alarm{display:none;color:#fff;background:#c00;padding:.15em .7em;border-radius:.3em;font-weight:bold;letter-spacing:.06em;animation:strobe .5s steps(2,end) infinite;vertical-align:middle;margin-left:.3em}
.alarm.on{display:inline-block}
.axis.estop{border-color:#f44;box-shadow:inset 0 0 0 2px #f44,0 0 14px rgba(255,40,40,.5);animation:frame .5s steps(2,end) infinite}
#net.estop{border-color:#f44;box-shadow:inset 0 0 0 2px #f44;animation:frame .5s steps(2,end) infinite}
@keyframes strobe{0%{filter:brightness(1)}50%{filter:brightness(.35)}100%{filter:brightness(1)}}
@keyframes sweep{to{transform:rotate(360deg)}}
@keyframes frame{0%{border-color:#f44}50%{border-color:#611}100%{border-color:#f44}}
.pbar{height:8px;background:#222;border:1px solid #444;border-radius:4px;margin-top:.3em;overflow:hidden}
.tfill{height:100%;width:0;background:#4a8;transition:width .6s linear}
.tfill.edge{background:#f44}
.msg{font-size:.8em;color:#fc3;min-height:1.2em}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.55}}
</style></head>
<body>
<h1>Antenna Tuner Controller</h1>
<div class="top">backend <b id="be">?</b> <span class="sep">·</span> ip <b id="ip">?</b> <span class="sep">·</span> link <b id="ln">?</b> <span class="sep">·</span> master clients <b id="mc">?</b> <span class="sep">·</span> homed <b id="hm">?</b> <span class="sep">·</span> fwd <b id="fw">?</b> W <span class="sep">·</span> settings <b id="stg">?</b> <a href="/api/settings" target="_blank" style="color:#8bf">view config</a> <a href="/api/settings?src=card" target="_blank" style="color:#8bf">read card</a> <button onclick="cmd('/api/settings_save','net')" style="padding:.1em .5em;font-size:.85em">save to card now</button> <span class="sep">·</span> build <b id="bld">?</b></div>

<div class="panel" id="net">
  <div class="row">
    <span id="stByp" class="state byp">BYPASS</span>
    <span id="stRf" class="state rf" style="display:none">RF LOCKOUT</span>
    <button class="big" id="bBypOn" onclick="cmd('/api/bypass?on=1','net')">Bypass (out of circuit)</button>
    <button class="big go" id="bBypOff" onclick="engage()">Engage network</button>
    <span class="sep">·</span>
    <span id="sideBox">side <button id="bHi" onclick="cmd('/api/side?v=hi_z','net')">Hi-Z (C on antenna side)</button> <button id="bLo" onclick="cmd('/api/side?v=lo_z','net')">Lo-Z (C on TX side)</button></span>
    <span class="sep">·</span>
    <button onclick="if(confirm('Drive every bound element back to its declared home (0)?'))cmd('/api/home','net')">Home all</button>
    <button class="estop mid" id="bEsAll" onclick="estopAllToggle()" title="press = stop and latch every motor · press again = release">EMERGENCY<br>STOP<br>ALL</button>
    <span class="beacon" id="bcnAll"></span>
    <span class="alarm" id="almAll">EMERGENCY STOP</span>
    <label class="kv"><input type="checkbox" id="buzz" checked> buzzer</label>
  </div>
  <div class="row kv">topology <b id="tk">?</b> <span id="tmap"></span> <span class="sep">·</span> fake RF <input type="number" id="fwin" value="0" min="0" step="1" style="width:5em"> W <button onclick="cmd('/api/fwd_w?w='+document.getElementById('fwin').value,'net')">Inject</button> <span class="kv">(lockout above 5 W — for testing the interlock)</span></div>
  <div class="msg" id="msg-net"></div>
</div>

<div id="axes"></div>

<div class="panel" id="topo">
  <h2>Topology / element → motor map</h2>
  <div class="row">
    kind <select id="tkind" onchange="renderTopoEditor()"><option value="balanced_l">Balanced L (L pair + C, Hi-Z / Lo-Z relays)</option><option value="balanced_pi">Balanced Pi (C1 + L pair + C2)</option></select>
    <span id="tedit"></span>
    <button onclick="applyTopo()">Apply</button>
    <span class="kv">requires bypass and no motion; persisted to EEPROM</span>
  </div>
  <div class="msg" id="msg-topo"></div>
</div>

<div class="panel" id="fwpanel">
  <h2>Firmware update over Ethernet</h2>
  <div class="row kv">running <b id="fwrun">?</b> <span class="sep">·</span> target <b id="fwt">?</b> <span class="sep">·</span> staged <b id="fws">?</b> <span id="fwi"></span></div>
  <div class="row">
    <input type="file" id="fwfile" accept=".hex">
    <button onclick="fwUpload()">Upload .hex</button>
    <button class="go" id="fwApply" onclick="fwApply()" disabled>Apply &amp; reboot</button>
    <button id="fwDiscard" onclick="cmd('/api/firmware_abort','fw')">Discard</button>
    <span class="kv">or <code>pio run -e teensy41_native_ota -t upload</code> · upload, discard and apply need every axis stopped; apply also needs bypass and no RF · the copy takes a few seconds — do not power-cycle · first install and recovery stay on USB</span>
  </div>
  <div class="msg" id="msg-fw"></div>
</div>

<script>
const KINDS=[['unset','— not set —'],['inductor','Roller inductor (stops)'],['vacuum_cap','Vacuum variable cap (stops)'],['varcap_limited','Variable cap with stops'],['varcap_free','Variable cap, free rotation'],['variometer','Variometer, free rotation']];
const LIMITED={inductor:1,vacuum_cap:1,varcap_limited:1};
const ELEMS={balanced_l:['L','C'],balanced_pi:['C1','L','C2']};
let lastMsg={},last=null;
async function cmd(u,tag){try{const r=await fetch(u);const t=(await r.text()).trim();if(tag){lastMsg[tag]=(r.ok?'':'✗ ')+t;}await poll();}catch(e){}}
function engage(){if(!last)return;const un=last.axes.filter(a=>a.name&&!a.anchored).map(a=>a.name);
  if(un.length&&!confirm('Element(s) '+un.join(', ')+' have no declared home. Engage the network anyway?'))return;
  cmd('/api/bypass?on=0','net');}
function jog(ax,n){if(!n||isNaN(n))return;cmd('/api/jog?axis='+ax+'&dir='+(n>0?'cw':'ccw')+'&steps='+Math.abs(n),ax);}
function jogN(ax,sgn){const v=parseInt(document.getElementById('j-'+ax).value);if(!(v>0))return;jog(ax,sgn*v);}
function rot(ax,sgn){const v=parseInt(document.getElementById('n-'+ax).value);if(!(v>0))return;cmd('/api/rotate?axis='+ax+'&revs='+v+'&dir='+(sgn>0?'cw':'ccw'),ax);}
function gotoPos(ax){const v=parseInt(document.getElementById('g-'+ax).value);if(isNaN(v))return;cmd('/api/goto?axis='+ax+'&steps='+v,ax);}
function setSpd(ax){const s=document.getElementById('s-'+ax).value,a=document.getElementById('a-'+ax).value;if(s==='')return;cmd('/api/speed?axis='+ax+'&v='+s+(a!==''?'&acc='+a:''),ax);}
function setElem(ax){const k=document.getElementById('k-'+ax).value;const m=document.getElementById('m-'+ax).value;
  if(LIMITED[k]&&!(parseFloat(m)>0)){lastMsg[ax]='✗ enter the rated travel (revolutions) for this element';render(last);return;}
  if(LIMITED[k]&&!confirm('Set axis '+ax+' as '+k+' with '+m+' rev of travel?\nThe window protects the device only after you declare home a few steps inside the physical stop.'))return;
  cmd('/api/element?axis='+ax+'&kind='+k+'&max_rev='+(m||0),ax);}
function zero(ax){if(confirm('Declare the CURRENT position of axis '+ax+' as home (0)?'))cmd('/api/zero?axis='+ax,ax);}
function estopToggle(ax){const a=last&&last.axes.find(x=>x.axis===ax);
  if(a&&a.estop){if(confirm('Release the emergency stop on motor '+ax+(a.name?' ('+a.name+')':'')+'?'))cmd('/api/estop_reset?axis='+ax,ax);}
  else cmd('/api/estop?axis='+ax,ax);}
function estopAllToggle(){const any=last&&last.axes.some(a=>a.estop);
  if(any){if(confirm('Release ALL emergency stops?'))cmd('/api/estop_reset','net');}
  else cmd('/api/estop','net');}
async function fwUpload(){const f=document.getElementById('fwfile').files[0];if(!f){lastMsg['fw']='✗ choose a .hex file first';render(last);return;}
  if(!confirm('Upload '+f.name+' ('+Math.round(f.size/1024)+' KB) to the controller?\nNothing is applied until you press Apply.'))return;
  lastMsg['fw']='uploading '+f.name+' …';render(last);
  try{const r=await fetch('/api/firmware',{method:'POST',body:f});const t=(await r.text()).trim();lastMsg['fw']=(r.ok?'':'✗ ')+t.replace(/\n/g,' · ');}catch(e){lastMsg['fw']='✗ upload failed: '+e;}
  await poll();}
function fwApply(){const o=last&&last.ota;if(!o||o.state!=='staged')return;
  if(!confirm('Apply the staged firmware ('+o.lines+' records, '+o.bytes+' bytes, crc32 '+o.crc32+') and reboot the controller?\nBypass must be engaged and nothing may be moving.'))return;
  cmd('/api/firmware_apply?lines='+o.lines,'fw');}
function unhome(ax){if(confirm('Unset home on axis '+ax+'? Travel window INACTIVE and the axis is unanchored until you set home again.'))cmd('/api/unhome?axis='+ax,ax);}
function drv(ax,on){if(!on&&!confirm('Disable the driver on axis '+ax+'? The element can then be turned by hand and the position counter will be wrong until you re-declare home.'))return;cmd('/api/enable?axis='+ax+'&on='+(on?1:0),ax);}
function syncField(el,v){if(!el||document.activeElement===el)return;const live=String(v);
  if(el.value===live){el.dataset.auto=live;return;}
  if(el.value===''||el.value===el.dataset.auto||el.dataset.auto===undefined){el.value=live;el.dataset.auto=live;}}
async function poll(){try{const r=await fetch('/api/status');render(await r.json());}catch(e){}}
function renderTopoEditor(){const k=document.getElementById('tkind').value;const ed=document.getElementById('tedit');
  const cur=last&&last.topology.kind===k?last.topology.elements:null;
  ed.innerHTML=ELEMS[k].map((n,i)=>{const a=cur?cur.find(e=>e.name===n):null;const v=a?a.axis:i;
    return n+' → motor <select id="te-'+n+'">'+[0,1,2].map(x=>'<option value="'+x+'"'+(x===v?' selected':'')+'>'+x+' ('+'XYZ'[x]+')</option>').join('')+'</select>';}).join(' &nbsp; ');}
function applyTopo(){const k=document.getElementById('tkind').value;
  const q=ELEMS[k].map(n=>n+'='+document.getElementById('te-'+n).value).join('&');
  if(!confirm('Apply topology '+k+' with map '+q.replace(/&/g,', ')+'?'))return;
  cmd('/api/topology?kind='+k+'&'+q,'topo');}
function render(s){
  if(!s)return;last=s;
  document.getElementById('be').textContent=s.net.backend;
  document.getElementById('ip').textContent=s.net.ip;
  document.getElementById('ln').textContent=s.net.link;
  document.getElementById('mc').textContent=s.net.master_clients;
  const hm=document.getElementById('hm');hm.textContent=s.homed?'YES':'NO';hm.style.color=s.homed?'#4f4':'#fc3';
  document.getElementById('fw').textContent=s.fwd_w.toFixed(1);
  const sb=document.getElementById('stByp');sb.textContent=s.bypass?'BYPASS — network out of circuit':'IN CIRCUIT';sb.className='state '+(s.bypass?'byp':'eng');
  document.getElementById('stRf').style.display=s.rf_lockout?'inline-block':'none';
  document.getElementById('bBypOn').classList.toggle('sel',s.bypass);
  document.getElementById('bBypOff').classList.toggle('sel',!s.bypass);
  const isL=s.topology.kind==='balanced_l';
  document.getElementById('sideBox').style.display=isL?'inline':'none';
  document.getElementById('bHi').classList.toggle('sel',s.side==='hi_z');
  document.getElementById('bLo').classList.toggle('sel',s.side==='lo_z');
  document.getElementById('tk').textContent=s.topology.kind;
  document.getElementById('tmap').textContent='('+s.topology.elements.map(e=>e.name+(e.pair?' pair':'')+' → motor '+e.axis+' '+'XYZ'[e.axis]).join(', ')+')';
  const anyE=s.estop_all||s.axes.some(a=>a.estop);
  document.getElementById('bcnAll').classList.toggle('on',anyE);
  const almAll=document.getElementById('almAll');almAll.classList.toggle('on',anyE);
  almAll.textContent=s.estop_all?'EMERGENCY STOP — ALL MOTORS':'EMERGENCY STOP — '+s.axes.filter(a=>a.estop).map(a=>a.name||('motor '+a.axis)).join(', ');
  const bAll=document.getElementById('bEsAll');bAll.classList.toggle('down',anyE);
  bAll.innerHTML=anyE?'RELEASE<br>E-STOP<br>ALL':'EMERGENCY<br>STOP<br>ALL';
  document.getElementById('net').classList.toggle('estop',anyE);
  const stg=document.getElementById('stg');const sg=s.settings||{};
  stg.textContent=sg.sd_present?('SD card'+(sg.sd_ok?' ✓':' ✗ write failed')+' + EEPROM'+(sg.source==='sd'?' (booted from card)':'')):'EEPROM only — no SD card';
  stg.style.color=sg.sd_present?(sg.sd_ok?'#4f4':'#f44'):'#fc3';
  buzzer(anyE);
  const bi=s.build||{},o=s.ota||{};document.getElementById('bld').textContent=(bi.stamp||'?')+' '+(bi.git||'');
  document.getElementById('fwrun').textContent=(bi.stamp||'?')+' · git '+(bi.git||'?')+' · '+(bi.env||'?');
  document.getElementById('fwt').textContent=o.target||'?';
  const fws=document.getElementById('fws');fws.textContent=o.state||'?';fws.style.color=o.state==='staged'?'#4f4':o.state==='error'?'#f44':o.state==='applying'?'#fc3':'#eee';
  document.getElementById('fwi').textContent=o.state==='staged'?'('+o.lines+' records, '+o.bytes+' bytes, '+o.min+'–'+o.max+', crc32 '+o.crc32+')'+(o.code?' — '+o.code+': '+o.msg:''):o.state==='error'?'('+o.code+': '+o.msg+')':o.state==='applying'?'— copying and rebooting (a few seconds, do not power-cycle); reload this page in ~20 s':'';
  document.getElementById('fwApply').disabled=o.state!=='staged'||s.moving;
  document.getElementById('fwDiscard').disabled=s.moving||o.state==='applying'||o.state==='idle';
  document.getElementById('msg-fw').textContent=lastMsg['fw']||'';
  document.getElementById('msg-net').textContent=lastMsg['net']||'';
  document.getElementById('msg-topo').textContent=lastMsg['topo']||'';
  const tk=document.getElementById('tkind');if(document.activeElement!==tk&&tk.dataset.auto!==s.topology.kind){tk.value=s.topology.kind;tk.dataset.auto=s.topology.kind;renderTopoEditor();}
  if(!document.getElementById('tedit').childElementCount)renderTopoEditor();
  const root=document.getElementById('axes');
  const order=[...s.axes].sort((a,b)=>(a.name?0:1)-(b.name?0:1)||a.axis-b.axis);
  const sig=order.map(a=>a.axis+':'+a.name).join('|');
  if(root.dataset.sig!==sig){
    root.dataset.sig=sig;root.innerHTML='';
    for(const a of order){
      const d=document.createElement('div');d.className='panel axis'+(a.name?'':' spare unbound');d.id='ax-'+a.axis;
      const opts=KINDS.map(k=>`<option value="${k[0]}">${k[1]}</option>`).join('');
      const title=a.name?`<b>${a.name}</b> ${a.name==='L'?'— inductor pair':'— capacitor'} <span class="kv">motor ${a.axis} (${a.letter})</span>`:`spare motor ${a.axis} (${a.letter}) <span class="kv">not bound by the topology</span>`;
      d.innerHTML=`
<h2><span class="beacon bcn"></span>${title}<span class="alarm alm">EMERGENCY STOP</span><span class="bdg dim unc">NO ELEMENT KIND — TRAVEL UNLIMITED</span><span class="bdg warn nohome">HOME NOT SET — UNANCHORED, SETUP MOVES IN BYPASS ONLY</span><span class="bdg run torun"></span><span class="bdg red limhit"></span><span class="bdg red dis">DRIVER DISABLED</span><span class="bdg red lsw">LIMIT SWITCH</span></h2>
<div class="kv">pos <b class="pos">?</b> steps <span class="sep">·</span> turn <b class="trn">?</b> <span class="mxr"></span> <span class="sep">·</span> travel <b class="trv">?</b> <span class="sep">·</span> kind <b class="knd">?</b> <span class="sep">·</span> Speed <b class="spd">?</b> steps/s <span class="sep">·</span> Accel <b class="acc">?</b> steps/s²</div>
<div class="pbar twin"><div class="tfill"></div></div>
<div class="msg"></div>
<div class="row">
  <button onclick="jog(${a.axis},-100)">−100</button><button onclick="jog(${a.axis},-10)">−10</button><button onclick="jog(${a.axis},-1)">−1 step</button>
  <button onclick="jog(${a.axis},1)">+1 step</button><button onclick="jog(${a.axis},10)">+10</button><button onclick="jog(${a.axis},100)">+100</button>
  <input type="number" id="j-${a.axis}" value="500" min="1" step="1" title="steps"><button onclick="jogN(${a.axis},-1)">−N steps</button><button onclick="jogN(${a.axis},1)">+N steps</button>
  <input type="number" id="g-${a.axis}" value="0" step="1" title="absolute steps"><button onclick="gotoPos(${a.axis})">Go to</button>
</div>
<div class="row">
  <button onclick="cmd('/api/rotate?axis=${a.axis}&revs=1&dir=ccw',${a.axis})">−1 rev</button><button onclick="cmd('/api/rotate?axis=${a.axis}&revs=1&dir=cw',${a.axis})">+1 rev</button>
  <input type="number" id="n-${a.axis}" value="5" min="1" title="revolutions"><button onclick="rot(${a.axis},-1)">−N rev</button><button onclick="rot(${a.axis},1)">+N rev</button>
  <button class="go" onclick="cmd('/api/run?axis=${a.axis}&dir=ccw',${a.axis})">◀ Run to home end</button><button class="go" onclick="cmd('/api/run?axis=${a.axis}&dir=cw',${a.axis})">Run to max end ▶</button>
  <button onclick="cmd('/api/stop?axis=${a.axis}',${a.axis})">Stop</button>
  <button class="estop es" onclick="estopToggle(${a.axis})">EMERGENCY<br>STOP</button>
</div>
<div class="row">
  element <select id="k-${a.axis}">${opts}</select> rated travel <input type="number" id="m-${a.axis}" min="0" step="0.5" title="revolutions"> rev
  <button onclick="setElem(${a.axis})">Set element</button>
  <button onclick="zero(${a.axis})">Set current pos as home</button><button onclick="unhome(${a.axis})">Unset home</button>
  <button onclick="drv(${a.axis},true)">Driver on</button><button onclick="drv(${a.axis},false)">Driver off</button>
</div>
<div class="row">
  Speed <input type="number" id="s-${a.axis}" min="1" max="200000" step="100" onkeydown="if(event.key==='Enter')setSpd(${a.axis})"> steps/s &nbsp; Accel <input type="number" id="a-${a.axis}" min="1" step="100" onkeydown="if(event.key==='Enter')setSpd(${a.axis})"> steps/s² <button onclick="setSpd(${a.axis})">Set (saved)</button>
</div>`;
      root.appendChild(d);
    }
  }
  for(const a of s.axes){
    const d=document.getElementById('ax-'+a.axis);if(!d)continue;
    const lim=(LIMITED[a.kind]&&a.home_set&&a.max_steps>0),hasStops=!!LIMITED[a.kind];
    d.querySelector('.bcn').classList.toggle('on',!!a.estop);
    d.querySelector('.alm').classList.toggle('on',!!a.estop);
    const es=d.querySelector('.es');es.classList.toggle('down',!!a.estop);es.innerHTML=a.estop?'RELEASE<br>E-STOP':'EMERGENCY<br>STOP';
    d.classList.toggle('estop',!!a.estop);
    d.querySelector('.unc').classList.toggle('on',a.kind==='unset');
    d.querySelector('.nohome').classList.toggle('on',!a.home_set);
    d.querySelector('.dis').classList.toggle('on',!a.enabled);
    d.querySelector('.lsw').classList.toggle('on',!!a.limit_sw);
    const atBound=(a.travel==='home'||a.travel==='max'||a.travel==='below_home'||a.travel==='above_max');
    const hit=d.querySelector('.limhit'),torun=d.querySelector('.torun');let showHit=false,showRun=false;
    if(lim&&a.last_clamp!=='none'){
      if(a.moving){torun.textContent='→ running to '+a.last_clamp.toUpperCase()+' limit';showRun=true;}
      else if(a.travel===a.last_clamp){hit.textContent='STOPPED AT '+a.last_clamp.toUpperCase()+' LIMIT';showHit=true;}
      else if(a.travel==='below_home'||a.travel==='above_max'){hit.textContent='OUTSIDE WINDOW ('+a.travel.replace('_',' ')+') — only moves back in are allowed';showHit=true;}
      else{torun.textContent='run to '+a.last_clamp.toUpperCase()+' limit interrupted';showRun=true;}
    }else if(lim&&(a.travel==='home'||a.travel==='max')){hit.textContent='AT '+a.travel.toUpperCase();showHit=true;}
    else if(lim&&(a.travel==='below_home'||a.travel==='above_max')){hit.textContent='OUTSIDE WINDOW ('+a.travel.replace('_',' ')+') — only moves back in are allowed';showHit=true;}
    hit.classList.toggle('on',showHit);torun.classList.toggle('on',showRun);d.classList.toggle('atlimit',showHit||!!a.limit_sw);
    d.querySelector('.pos').textContent=a.steps+(a.moving?' ▸':'');
    d.querySelector('.trn').textContent=a.turns.toFixed(3);
    d.querySelector('.mxr').textContent=hasStops?'/ '+a.max_rev.toFixed(2)+' rev':'rev (no stops)';
    const trv=d.querySelector('.trv');trv.textContent=a.travel.replace('_',' ');trv.style.color=(lim&&atBound)?'#f44':(a.travel==='unhomed'?'#fc3':'#eee');
    d.querySelector('.knd').textContent=a.kind;
    const tw=d.querySelector('.twin'),tf=d.querySelector('.tfill');
    if(hasStops&&a.max_steps>0){tw.style.display='block';const f=Math.max(0,Math.min(1,a.steps/a.max_steps));tf.style.width=(100*f)+'%';tf.className='tfill'+((lim&&atBound)?' edge':'');}else tw.style.display='none';
    d.querySelector('.spd').textContent=a.speed;d.querySelector('.acc').textContent=a.accel;
    d.querySelector('.msg').textContent=lastMsg[a.axis]||'';
    syncField(document.getElementById('s-'+a.axis),a.speed);
    syncField(document.getElementById('a-'+a.axis),a.accel);
    const ks=document.getElementById('k-'+a.axis);
    if(ks&&document.activeElement!==ks){if(ks.value===a.kind){ks.dataset.auto=a.kind;}else if(ks.dataset.auto===undefined||ks.value===ks.dataset.auto){ks.value=a.kind;ks.dataset.auto=a.kind;}}
    syncField(document.getElementById('m-'+a.axis),hasStops?a.max_rev:'');
  }
}
// Buzzer: two-tone industrial alarm while any E-stop is latched. Browsers
// only allow audio after a user gesture, so the first tone may wait for
// the click that raised the alarm; the checkbox mutes it.
let actx=null,buzzTimer=null;
function beep(f,ms){try{if(!actx)actx=new (window.AudioContext||window.webkitAudioContext)();
  const o=actx.createOscillator(),g=actx.createGain();o.type='square';o.frequency.value=f;g.gain.value=.08;
  o.connect(g);g.connect(actx.destination);o.start();o.stop(actx.currentTime+ms/1000);}catch(e){}}
function buzzer(on){const want=on&&document.getElementById('buzz').checked;
  if(want&&!buzzTimer){beep(880,180);buzzTimer=setInterval(()=>{beep(880,180);setTimeout(()=>beep(660,180),250);},1000);}
  if(!want&&buzzTimer){clearInterval(buzzTimer);buzzTimer=null;}}
poll();setInterval(poll,1000);
</script>
</body></html>)HTML";

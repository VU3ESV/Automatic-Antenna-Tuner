#!/usr/bin/env python3
"""Flash the tuner-controller over Ethernet.

Stages PlatformIO's firmware.hex on the running controller
(POST /api/firmware), compares the controller's CRC-32 of the staged image
with the one computed here from the same file, asks it to apply
(GET /api/firmware_apply?lines=N — N repeats the record count, the same
confirmation FlasherX asks for on a serial console) and waits for the
controller to come back, reporting the build stamp it now runs.

    ota_upload.py --host 192.168.86.44 .pio/build/teensy41_native/firmware.hex
    ota_upload.py --host 192.168.86.44 --stage-only firmware.hex   # stage, do not apply

Used by the *_ota PlatformIO environments as upload_command. Standard
library only. Exit status is non-zero on any failure so `pio run -t upload`
reports it.

The controller refuses to start an upload while an axis is moving and
refuses to apply unless bypass is engaged, nothing moves and no RF is
present; those refusals are printed verbatim. The first install and any
recovery from a bad image remain USB (teensy_loader_cli).
"""
import argparse
import json
import sys
import time
import urllib.error
import urllib.request
import zlib


def parse_hex(path):
    """Return (record_count, lo, hi, image) — image is [lo, hi) with 0xFF gaps.

    Mirrors app/ota.cpp::process_line exactly so the two sides count the
    same `lines` and reject the same files: '\r' is ignored, '\n' ends a
    record, blank lines are skipped, no other whitespace is tolerated;
    every record up to and including EOF counts; nothing but blank lines
    may follow EOF; data records must be ascending and non-overlapping;
    extended address records carry 2 bytes, start address records 4."""
    lines = 0
    base = 0
    chunks = []
    lo = None
    hi = 0
    eof = False
    with open(path, "rb") as f:
        text = f.read()
    for s in text.replace(b"\r", b"").split(b"\n"):
        if not s:
            continue
        no = lines + 1
        if s[:1] != b":":
            sys.exit(f"{path}: record {no} does not start with ':' — not an Intel HEX file")
        if len(s) < 11 or (len(s) - 1) % 2:
            sys.exit(f"{path}: record {no} is malformed")
        try:
            rec = bytes.fromhex(s[1:].decode("ascii"))
        except (ValueError, UnicodeDecodeError):
            sys.exit(f"{path}: record {no} has a bad hex digit")
        if rec[0] != len(rec) - 5:
            sys.exit(f"{path}: record {no} length field does not match")
        if sum(rec) & 0xFF:
            sys.exit(f"{path}: record {no} checksum mismatch")
        lines += 1
        if eof:
            sys.exit(f"{path}: record {no} follows the EOF record")
        n, addr, rtype, data = rec[0], (rec[1] << 8) | rec[2], rec[3], rec[4:-1]
        if rtype == 0x00:
            a = base + addr
            if lo is not None and a < hi:
                sys.exit(f"{path}: record {no} at 0x{a:08X} overlaps or precedes earlier data")
            if n:
                chunks.append((a, data))
                lo = a if lo is None else min(lo, a)
                hi = max(hi, a + n)
        elif rtype == 0x01:
            eof = True
        elif rtype in (0x02, 0x04):
            if n != 2:
                sys.exit(f"{path}: record {no}: extended address record must carry 2 bytes")
            base = ((data[0] << 8) | data[1]) << (4 if rtype == 0x02 else 16)
        elif rtype in (0x03, 0x05):
            if n != 4:
                sys.exit(f"{path}: record {no}: start address record must carry 4 bytes")
        else:
            sys.exit(f"{path}: record {no} has unknown type {rtype:#04x}")
    if not eof:
        sys.exit(f"{path}: no EOF record")
    if lo is None:
        sys.exit(f"{path}: no data records")
    img = bytearray(b"\xff" * (hi - lo))
    for a, d in chunks:
        img[a - lo:a - lo + len(d)] = d
    return lines, lo, hi, bytes(img)


def http(host, port, method, path, body=None, timeout=30):
    req = urllib.request.Request(f"http://{host}:{port}{path}", data=body, method=method)
    if body is not None:
        req.add_header("Content-Type", "text/plain")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read().decode(errors="replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode(errors="replace")


def get_status(host, port, timeout=5):
    try:
        code, txt = http(host, port, "GET", "/api/status", timeout=timeout)
        return json.loads(txt) if code == 200 else None
    except (urllib.error.URLError, OSError, TimeoutError, ValueError):
        return None   # not up yet, or a reply cut short while the stack comes up


def describe(st):
    b = st.get("build", {})
    return f"{b.get('stamp', '?')} (git {b.get('git', '?')}, env {b.get('env', '?')})"


def main():
    ap = argparse.ArgumentParser(description="Flash the tuner-controller over Ethernet")
    ap.add_argument("hexfile")
    ap.add_argument("--host", required=True, help="controller IP or name")
    ap.add_argument("--port", type=int, default=80)
    ap.add_argument("--stage-only", action="store_true", help="upload and verify, do not apply")
    ap.add_argument("--wait", type=int, default=90, help="seconds to wait for the controller after apply")
    args = ap.parse_args()

    lines, lo, hi, img = parse_hex(args.hexfile)
    crc = zlib.crc32(img) & 0xFFFFFFFF
    print(f"{args.hexfile}: {lines} records, {len(img)} bytes, 0x{lo:08X}-0x{hi:08X}, crc32 {crc:08x}")

    before = get_status(args.host, args.port)
    if before is None:
        sys.exit(f"{args.host}: controller not reachable (GET /api/status failed)")
    print(f"controller runs: {describe(before)}")
    ota = before.get("ota", {})
    if ota.get("supported") is False:
        sys.exit("controller reports no firmware-update support")
    if before.get("moving"):
        sys.exit("an axis is moving — stop it first (the controller refuses uploads while moving)")
    if not args.stage_only and not before.get("bypass", False):
        sys.exit("bypass is not engaged — engage bypass first (the controller refuses to apply otherwise)")

    print("uploading …", flush=True)
    t0 = time.monotonic()
    try:
        code, txt = http(args.host, args.port, "POST", "/api/firmware", body=open(args.hexfile, "rb").read(), timeout=180)
    except (urllib.error.URLError, OSError, TimeoutError) as e:
        sys.exit(f"upload failed: {e}")
    print(f"controller replied {code} after {time.monotonic() - t0:.1f} s:")
    for ln in txt.strip().splitlines():
        print("   ", ln)
    if code != 200:
        sys.exit(1)
    fields = dict(ln.split("=", 1) for ln in txt.strip().splitlines() if "=" in ln)
    dev_lines = int(fields.get("lines", "0"))
    dev_crc = int(fields.get("crc32", "0"), 16)
    if dev_lines != lines or dev_crc != crc:
        try:
            http(args.host, args.port, "GET", "/api/firmware_abort")
            note = "discarded"
        except (urllib.error.URLError, OSError, TimeoutError) as e:
            note = f"could not discard it ({e}); the controller will discard it on the next upload"
        sys.exit(f"staged image does not match: controller lines={dev_lines} crc32={dev_crc:08x}, "
                 f"file lines={lines} crc32={crc:08x} — {note}")
    print("staged image verified: record count and CRC-32 match the file")

    if args.stage_only:
        print(f"not applied (--stage-only). To apply: GET http://{args.host}:{args.port}/api/firmware_apply?lines={lines}")
        return

    try:
        code, txt = http(args.host, args.port, "GET", f"/api/firmware_apply?lines={lines}")
    except (urllib.error.URLError, OSError, TimeoutError) as e:
        sys.exit(f"apply request failed: {e}")
    print(f"apply: {code} {txt.strip()}")
    if code != 200:
        sys.exit(1)

    # The copy is erase-dominated (~134 sector erases for a 270 KB image):
    # several seconds typical, tens of seconds worst case. Do not power-cycle.
    print("waiting for the controller to copy the image and reboot (several seconds) …", flush=True)
    time.sleep(6)
    deadline = time.monotonic() + args.wait
    after = None
    while time.monotonic() < deadline:
        after = get_status(args.host, args.port, timeout=3)
        if after is not None:
            break
        time.sleep(2)
    if after is None:
        sys.exit(f"controller did not answer within {args.wait} s — check the serial console / power cycle; "
                 "USB recovery: teensy_loader_cli")
    print(f"controller is back: {describe(after)}")
    if after.get("build") == before.get("build"):
        print("WARNING: the build stamp did not change — did the update apply?")
    ota_after = after.get("ota", {})
    if ota_after.get("state") == "staged" and ota_after.get("code") == "apply_aborted":
        sys.exit(f"the controller cancelled the apply: {ota_after.get('msg')} — the image is still staged; "
                 f"re-run once the tuner is inert")
    s = after.get("settings", {})
    print(f"settings source {s.get('source')}, SD {'present' if s.get('sd_present') else 'absent'}, "
          f"bypass {after.get('bypass')}, homed {after.get('homed')}, "
          f"positions {[a.get('steps') for a in after.get('axes', [])]}")


if __name__ == "__main__":
    main()

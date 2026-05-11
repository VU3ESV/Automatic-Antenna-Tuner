package protocol

import (
	"encoding/json"
	"testing"
	"time"
)

func TestParseCommand_Valid(t *testing.T) {
	raw := []byte(`{"type":"command","id":"abc-7","action":"set_bypass","args":{"bypass":true}}`)
	cmd, err := ParseCommand(raw)
	if err != nil {
		t.Fatalf("ParseCommand: %v", err)
	}
	if cmd.Action != "set_bypass" {
		t.Errorf("action = %q, want set_bypass", cmd.Action)
	}
	if cmd.ID != "abc-7" {
		t.Errorf("id = %q, want abc-7", cmd.ID)
	}
	// Args remain as RawMessage; per-verb decode happens at the dispatcher.
	var args struct{ Bypass bool }
	if err := json.Unmarshal(cmd.Args, &args); err != nil {
		t.Fatalf("decode args: %v", err)
	}
	if !args.Bypass {
		t.Errorf("args.bypass = false, want true")
	}
}

func TestParseCommand_WrongType(t *testing.T) {
	_, err := ParseCommand([]byte(`{"type":"telemetry","action":"x"}`))
	if err == nil {
		t.Fatal("expected error for non-command type")
	}
}

func TestParseCommand_MissingAction(t *testing.T) {
	_, err := ParseCommand([]byte(`{"type":"command","id":"x"}`))
	if err == nil {
		t.Fatal("expected error for missing action")
	}
}

func TestParseCommand_Malformed(t *testing.T) {
	_, err := ParseCommand([]byte(`{not json`))
	if err == nil {
		t.Fatal("expected error for malformed JSON")
	}
}

func TestAckPreservesID(t *testing.T) {
	cmd := Command{Type: TypeCommand, ID: "xyz", Action: "noop"}
	ok := NewAckOK(cmd)
	if ok.Ref != "xyz" || !ok.OK || ok.Err != nil {
		t.Errorf("NewAckOK = %+v", ok)
	}
	bad := NewAckErr(cmd, CodeBadArgs, "missing delta_steps")
	if bad.Ref != "xyz" || bad.OK || bad.Err == nil {
		t.Errorf("NewAckErr = %+v", bad)
	}
	if bad.Err.Code != CodeBadArgs {
		t.Errorf("err.code = %q, want bad_args", bad.Err.Code)
	}
}

func TestTelemetryRoundTrip(t *testing.T) {
	swr := 1.13
	tel := Telemetry{
		Envelope: Envelope{Type: TypeTelemetry, Seq: 42, TS: time.Unix(1715420000, 0).UTC()},
		Data: TelemetryData{
			FwdW: 5.0, RevW: 0.04, SWR: &swr,
			ZMag: 56.2, ZPhase: -8.4, R: 55.6, X: -8.2, Mode: ModeRX,
		},
	}
	raw, err := json.Marshal(tel)
	if err != nil {
		t.Fatalf("marshal: %v", err)
	}
	var back Telemetry
	if err := json.Unmarshal(raw, &back); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	if back.Seq != 42 || back.Data.Mode != ModeRX {
		t.Errorf("round-trip lost fields: %+v", back)
	}
	if back.Data.SWR == nil || *back.Data.SWR != 1.13 {
		t.Errorf("SWR round-trip wrong: %+v", back.Data.SWR)
	}
}

func TestSWRInfinityIsNull(t *testing.T) {
	tel := Telemetry{Data: TelemetryData{SWR: nil}}
	raw, _ := json.Marshal(tel)
	got := string(raw)
	if !contains(got, `"swr":null`) {
		t.Errorf("expected swr:null in %s", got)
	}
}

func contains(s, sub string) bool {
	for i := 0; i+len(sub) <= len(s); i++ {
		if s[i:i+len(sub)] == sub {
			return true
		}
	}
	return false
}

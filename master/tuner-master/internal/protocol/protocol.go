// Package protocol defines the wire format documented in
// docs/PROTOCOL.md. All frames going onto a WebSocket — between
// browser/master and master/controller — round-trip through this
// package.
package protocol

import (
	"encoding/json"
	"errors"
	"fmt"
	"time"
)

// FrameType is the server→client `type` discriminator.
type FrameType string

const (
	TypeTelemetry FrameType = "telemetry"
	TypeState     FrameType = "state"
	TypeMemory    FrameType = "memory"
	TypeQRG       FrameType = "qrg"
	TypeStatus    FrameType = "status"
	TypeHeartbeat FrameType = "heartbeat"
	TypeAck       FrameType = "ack"
	TypeCommand   FrameType = "command" // client→server only
)

// Side selects the L-network capacitor topology.
type Side string

const (
	SideHiZ Side = "hi_z"
	SideLoZ Side = "lo_z"
)

// Mode is derived from forward power.
type Mode string

const (
	ModeRX   Mode = "rx"
	ModeTX   Mode = "tx"
	ModeTune Mode = "tune"
)

// StatusLevel and StatusCode are the stable enums used by both `status`
// frames and `ack.err` payloads (see PROTOCOL.md §2.5 / §3.1).
type StatusLevel string

const (
	StatusInfo  StatusLevel = "info"
	StatusWarn  StatusLevel = "warn"
	StatusError StatusLevel = "error"
)

type StatusCode string

const (
	CodeRFLockout     StatusCode = "rf_lockout"
	CodeEncResync     StatusCode = "enc_resync"
	CodeStall         StatusCode = "stall"
	CodeRelayFault    StatusCode = "relay_fault"
	CodeLinkLost      StatusCode = "link_lost"
	CodeCATStale      StatusCode = "cat_stale"
	CodeCalMissing    StatusCode = "cal_missing"
	CodeWSOverrun     StatusCode = "ws_overrun"
	CodeUnknownAction StatusCode = "unknown_action"
	CodeBadArgs       StatusCode = "bad_args"
)

// Envelope is what server→client frames have in common.
type Envelope struct {
	Type FrameType `json:"type"`
	Seq  uint32    `json:"seq"`
	TS   time.Time `json:"ts"`
}

// Telemetry — PROTOCOL.md §2.1.
type Telemetry struct {
	Envelope
	Data TelemetryData `json:"data"`
}

type TelemetryData struct {
	FwdW   float64  `json:"fwd_w"`
	RevW   float64  `json:"rev_w"`
	SWR    *float64 `json:"swr"` // nil = infinite
	ZMag   float64  `json:"z_mag"`
	ZPhase float64  `json:"z_phase"`
	R      float64  `json:"r"`
	X      float64  `json:"x"`
	Mode   Mode     `json:"mode"`
}

// State — PROTOCOL.md §2.2.
type State struct {
	Envelope
	Data StateData `json:"data"`
}

type StateData struct {
	LSteps   uint32    `json:"l_steps"`
	CSteps   uint32    `json:"c_steps"`
	LEnc     int32     `json:"l_enc"`
	CEnc     int32     `json:"c_enc"`
	Side     Side      `json:"side"`
	Bypass   bool      `json:"bypass"`
	LastMove time.Time `json:"last_move"`
	Moving   bool      `json:"moving"`
	Homed    bool      `json:"homed"`
}

// Status — PROTOCOL.md §2.5.
type Status struct {
	Envelope
	Level StatusLevel `json:"level"`
	Code  StatusCode  `json:"code,omitempty"`
	Msg   string      `json:"msg"`
}

// Heartbeat — PROTOCOL.md §2.6.
type Heartbeat struct {
	Envelope
}

// Ack — PROTOCOL.md §2.7.
type Ack struct {
	Type FrameType `json:"type"`
	Ref  string    `json:"ref,omitempty"`
	OK   bool      `json:"ok"`
	Err  *AckErr   `json:"err,omitempty"`
}

type AckErr struct {
	Code StatusCode `json:"code"`
	Msg  string     `json:"msg"`
}

// Command is the only client→server frame — PROTOCOL.md §3.
type Command struct {
	Type   FrameType       `json:"type"`
	ID     string          `json:"id,omitempty"`
	Action string          `json:"action"`
	Args   json.RawMessage `json:"args,omitempty"`
}

// ParseCommand validates the envelope of an inbound client frame.
// Args are left as RawMessage; per-verb decoding happens at the
// dispatcher.
func ParseCommand(raw []byte) (Command, error) {
	var c Command
	if err := json.Unmarshal(raw, &c); err != nil {
		return c, fmt.Errorf("parse command: %w", err)
	}
	if c.Type != TypeCommand {
		return c, fmt.Errorf("expected type=command, got %q", c.Type)
	}
	if c.Action == "" {
		return c, errors.New("command missing action")
	}
	return c, nil
}

// NewAckOK builds a positive ack for a Command (preserving its id).
func NewAckOK(cmd Command) Ack {
	return Ack{Type: TypeAck, Ref: cmd.ID, OK: true}
}

// NewAckErr builds a negative ack.
func NewAckErr(cmd Command, code StatusCode, msg string) Ack {
	return Ack{
		Type: TypeAck,
		Ref:  cmd.ID,
		OK:   false,
		Err:  &AckErr{Code: code, Msg: msg},
	}
}

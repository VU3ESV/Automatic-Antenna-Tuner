// Package state owns the master's last-known snapshot of the
// controller + master-synthesised data (memory, qrg). One writer, many
// readers, locked by a sync.RWMutex.
//
// The state core is intentionally dumb: it stores frames, broadcasts
// change events to subscribers, and never produces protocol bytes
// itself. The hub package handles WebSocket I/O.
package state

import (
	"sync"
	"time"

	"github.com/VU3ESV/Automatic-Antenna-Tuner/master/tuner-master/internal/protocol"
)

// Snapshot is the master's current understanding of the world. Each
// field can be updated independently as new frames arrive.
type Snapshot struct {
	Telemetry      *protocol.TelemetryData
	State          *protocol.StateData
	QRG            *QRGData
	ControllerLink LinkStatus
	UpdatedAt      time.Time
}

// QRGData mirrors protocol.qrg's payload locally so the state core
// does not depend on a wire `Envelope`.
type QRGData struct {
	FreqHz uint64    `json:"freq_hz"`
	Mode   string    `json:"mode,omitempty"`
	Rig    string    `json:"rig,omitempty"`
	Stale  bool      `json:"stale"`
	At     time.Time `json:"-"`
}

// LinkStatus reflects the master↔controller WS health.
type LinkStatus string

const (
	LinkConnecting   LinkStatus = "connecting"
	LinkConnected    LinkStatus = "connected"
	LinkDisconnected LinkStatus = "disconnected"
)

// Event is what subscribers receive. Kind matches the wire frame type
// where applicable; "link" is master-internal.
type Event struct {
	Kind protocol.FrameType
	// Snapshot at the moment of emission. Subscribers must treat it
	// as read-only.
	Snap Snapshot
}

// Core is the state hub.
type Core struct {
	mu       sync.RWMutex
	snap     Snapshot
	subs     map[chan<- Event]struct{}
}

// New returns a Core in the initial state: bypass engaged, link
// disconnected.
func New() *Core {
	return &Core{
		snap: Snapshot{
			State: &protocol.StateData{
				Side:   protocol.SideHiZ,
				Bypass: true, // invariant #2: bypass on power-up.
				Homed:  false,
			},
			ControllerLink: LinkDisconnected,
			UpdatedAt:      time.Now(),
		},
		subs: make(map[chan<- Event]struct{}),
	}
}

// Subscribe registers a channel for change events. The returned
// unsubscribe func must be called when the subscriber is done; the
// channel is closed by Core when it's unsubscribed.
//
// The caller's channel should be buffered (recommended cap: 16). If
// it blocks, Core drops the event (and emits a ws_overrun status —
// surfaced via UpdateStatus, not this fast path).
func (c *Core) Subscribe(ch chan<- Event) (unsubscribe func()) {
	c.mu.Lock()
	c.subs[ch] = struct{}{}
	c.mu.Unlock()
	return func() {
		c.mu.Lock()
		if _, ok := c.subs[ch]; ok {
			delete(c.subs, ch)
			close(ch)
		}
		c.mu.Unlock()
	}
}

// Snapshot returns a read-only copy of the current state.
func (c *Core) Snapshot() Snapshot {
	c.mu.RLock()
	defer c.mu.RUnlock()
	return c.snap
}

// UpdateTelemetry stores a new telemetry frame and notifies subscribers.
func (c *Core) UpdateTelemetry(t protocol.TelemetryData) {
	c.mu.Lock()
	c.snap.Telemetry = &t
	c.snap.UpdatedAt = time.Now()
	snap := c.snap
	c.mu.Unlock()
	c.broadcast(Event{Kind: protocol.TypeTelemetry, Snap: snap})
}

// UpdateState stores a new state frame.
func (c *Core) UpdateState(s protocol.StateData) {
	c.mu.Lock()
	c.snap.State = &s
	c.snap.UpdatedAt = time.Now()
	snap := c.snap
	c.mu.Unlock()
	c.broadcast(Event{Kind: protocol.TypeState, Snap: snap})
}

// UpdateQRG stores the latest transceiver frequency.
func (c *Core) UpdateQRG(q QRGData) {
	c.mu.Lock()
	q.At = time.Now()
	c.snap.QRG = &q
	c.snap.UpdatedAt = time.Now()
	snap := c.snap
	c.mu.Unlock()
	c.broadcast(Event{Kind: protocol.TypeQRG, Snap: snap})
}

// SetLink updates the master↔controller link health.
func (c *Core) SetLink(ls LinkStatus) {
	c.mu.Lock()
	if c.snap.ControllerLink == ls {
		c.mu.Unlock()
		return
	}
	c.snap.ControllerLink = ls
	c.snap.UpdatedAt = time.Now()
	snap := c.snap
	c.mu.Unlock()
	c.broadcast(Event{Kind: protocol.TypeStatus, Snap: snap})
}

func (c *Core) broadcast(ev Event) {
	c.mu.RLock()
	defer c.mu.RUnlock()
	for ch := range c.subs {
		select {
		case ch <- ev:
		default:
			// Subscriber too slow — drop. The hub layer is responsible
			// for closing the connection if this happens repeatedly.
		}
	}
}

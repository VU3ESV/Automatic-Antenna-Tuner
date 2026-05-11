// Package hub is the master's WebSocket fan-out to browser/touch
// clients. It subscribes to the state Core and pushes wire frames to
// each client; commands inbound from clients are routed to the
// dispatcher func supplied at construction.
package hub

import (
	"context"
	"encoding/json"
	"log/slog"
	"net/http"
	"sync/atomic"
	"time"

	"github.com/VU3ESV/Automatic-Antenna-Tuner/master/tuner-master/internal/protocol"
	"github.com/VU3ESV/Automatic-Antenna-Tuner/master/tuner-master/internal/state"
	"github.com/gorilla/websocket"
)

// CommandHandler is what the hub calls for each inbound client
// command. Returning an error converts to a negative ack with code
// `bad_args`; returning ok=false with a code/msg means "well-formed
// but refused" (e.g. rf_lockout).
type CommandHandler func(ctx context.Context, cmd protocol.Command) (ok bool, code protocol.StatusCode, msg string)

// Hub serves /ws and broadcasts state changes.
type Hub struct {
	core      *state.Core
	handler   CommandHandler
	heartbeat time.Duration
	upgrader  websocket.Upgrader
	seq       atomic.Uint32
}

// New constructs a Hub. heartbeat is how often a heartbeat frame is
// sent when no other frame would be.
func New(core *state.Core, handler CommandHandler, heartbeat time.Duration) *Hub {
	return &Hub{
		core:      core,
		handler:   handler,
		heartbeat: heartbeat,
		upgrader: websocket.Upgrader{
			// LAN-only deployment; cross-origin is fine.
			CheckOrigin: func(_ *http.Request) bool { return true },
		},
	}
}

// ServeWS is the http.HandlerFunc for /ws.
func (h *Hub) ServeWS(w http.ResponseWriter, r *http.Request) {
	conn, err := h.upgrader.Upgrade(w, r, nil)
	if err != nil {
		slog.Warn("ws upgrade failed", "err", err, "remote", r.RemoteAddr)
		return
	}
	slog.Info("ws client connected", "remote", r.RemoteAddr)

	c := &client{
		hub:    h,
		conn:   conn,
		send:   make(chan []byte, 32),
		remote: r.RemoteAddr,
	}
	go c.writeLoop()
	go c.readLoop(r.Context())
}

func (h *Hub) nextSeq() uint32 {
	for {
		s := h.seq.Load()
		next := s + 1
		if next == 0 { // wrap
			next = 1
		}
		if h.seq.CompareAndSwap(s, next) {
			return next
		}
	}
}

// envelope builds the common server→client header.
func (h *Hub) envelope(t protocol.FrameType) protocol.Envelope {
	return protocol.Envelope{Type: t, Seq: h.nextSeq(), TS: time.Now().UTC()}
}

type client struct {
	hub    *Hub
	conn   *websocket.Conn
	send   chan []byte
	remote string
}

func (c *client) writeLoop() {
	ev := make(chan state.Event, 16)
	unsub := c.hub.core.Subscribe(ev)
	defer func() {
		unsub()
		_ = c.conn.Close()
	}()

	// Send a snapshot immediately so the client doesn't have to wait
	// for the next change.
	c.sendCurrentSnapshot()

	tick := time.NewTicker(c.hub.heartbeat)
	defer tick.Stop()

	for {
		select {
		case raw, ok := <-c.send:
			if !ok {
				return
			}
			if err := c.write(raw); err != nil {
				slog.Debug("ws write failed", "remote", c.remote, "err", err)
				return
			}
		case e, ok := <-ev:
			if !ok {
				return
			}
			c.deliver(e)
		case <-tick.C:
			hb := protocol.Heartbeat{Envelope: c.hub.envelope(protocol.TypeHeartbeat)}
			b, _ := json.Marshal(hb)
			if err := c.write(b); err != nil {
				return
			}
		}
	}
}

func (c *client) write(raw []byte) error {
	_ = c.conn.SetWriteDeadline(time.Now().Add(5 * time.Second))
	return c.conn.WriteMessage(websocket.TextMessage, raw)
}

func (c *client) deliver(e state.Event) {
	var raw []byte
	switch e.Kind {
	case protocol.TypeTelemetry:
		if e.Snap.Telemetry == nil {
			return
		}
		raw, _ = json.Marshal(protocol.Telemetry{
			Envelope: c.hub.envelope(protocol.TypeTelemetry),
			Data:     *e.Snap.Telemetry,
		})
	case protocol.TypeState:
		if e.Snap.State == nil {
			return
		}
		raw, _ = json.Marshal(protocol.State{
			Envelope: c.hub.envelope(protocol.TypeState),
			Data:     *e.Snap.State,
		})
	case protocol.TypeQRG:
		if e.Snap.QRG == nil {
			return
		}
		// Pack QRG via a small inline struct so we don't bleed
		// state.QRGData JSON tags onto the wire.
		raw, _ = json.Marshal(struct {
			protocol.Envelope
			Data state.QRGData `json:"data"`
		}{c.hub.envelope(protocol.TypeQRG), *e.Snap.QRG})
	case protocol.TypeStatus:
		raw, _ = json.Marshal(protocol.Status{
			Envelope: c.hub.envelope(protocol.TypeStatus),
			Level:    protocol.StatusInfo,
			Code:     protocol.CodeLinkLost,
			Msg:      string(e.Snap.ControllerLink),
		})
	default:
		return
	}
	if err := c.write(raw); err != nil {
		slog.Debug("ws deliver failed", "remote", c.remote, "err", err)
	}
}

func (c *client) sendCurrentSnapshot() {
	snap := c.hub.core.Snapshot()
	if snap.State != nil {
		raw, _ := json.Marshal(protocol.State{
			Envelope: c.hub.envelope(protocol.TypeState),
			Data:     *snap.State,
		})
		_ = c.write(raw)
	}
	if snap.Telemetry != nil {
		raw, _ := json.Marshal(protocol.Telemetry{
			Envelope: c.hub.envelope(protocol.TypeTelemetry),
			Data:     *snap.Telemetry,
		})
		_ = c.write(raw)
	}
}

func (c *client) readLoop(ctx context.Context) {
	defer c.conn.Close()
	c.conn.SetReadLimit(64 * 1024)
	for {
		_, raw, err := c.conn.ReadMessage()
		if err != nil {
			if websocket.IsUnexpectedCloseError(err, websocket.CloseNormalClosure, websocket.CloseGoingAway) {
				slog.Debug("ws read error", "remote", c.remote, "err", err)
			}
			return
		}
		cmd, err := protocol.ParseCommand(raw)
		if err != nil {
			slog.Debug("ws command parse", "remote", c.remote, "err", err)
			// Best-effort negative ack with empty ref.
			ack, _ := json.Marshal(protocol.NewAckErr(protocol.Command{}, protocol.CodeBadArgs, err.Error()))
			_ = c.write(ack)
			continue
		}
		var ack protocol.Ack
		if c.hub.handler == nil {
			ack = protocol.NewAckErr(cmd, protocol.CodeUnknownAction, "no handler installed")
		} else {
			ok, code, msg := c.hub.handler(ctx, cmd)
			if ok {
				ack = protocol.NewAckOK(cmd)
			} else {
				ack = protocol.NewAckErr(cmd, code, msg)
			}
		}
		raw, _ = json.Marshal(ack)
		if err := c.write(raw); err != nil {
			return
		}
	}
}

// NoopHandler accepts every command. Useful for M1 development before
// real verb routing exists.
func NoopHandler(_ context.Context, _ protocol.Command) (bool, protocol.StatusCode, string) {
	return true, "", ""
}

// Compile-time guard.
var _ http.HandlerFunc = (&Hub{}).ServeWS

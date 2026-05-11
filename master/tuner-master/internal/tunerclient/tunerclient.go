// Package tunerclient is the master-side TCP client for the tuner
// controller's protocol server (docs/PROTOCOL.md §1.0 — "Master ↔
// Controller: Plain TCP on port 8089, one protocol frame per line").
//
// One goroutine per Run() call manages a single persistent connection
// with auto-reconnect. Inbound frames are decoded with the existing
// `protocol` types and pushed into the state.Core. Outbound commands
// are accepted via Send() and written line-framed.
//
// The same package replaces internal/fakecontroller in main.go once the
// real Teensy controller is reachable.
package tunerclient

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"net"
	"strconv"
	"sync"
	"time"

	"github.com/VU3ESV/Automatic-Antenna-Tuner/master/tuner-master/internal/protocol"
	"github.com/VU3ESV/Automatic-Antenna-Tuner/master/tuner-master/internal/state"
)

// Options configures the client.
type Options struct {
	Host            string        // tuner-controller IP / hostname
	Port            int           // TCP port (default 8089)
	InitialBackoff  time.Duration // first reconnect delay (default 1 s)
	MaxBackoff      time.Duration // backoff ceiling (default 30 s)
	ReadIdleTimeout time.Duration // close+reconnect if no bytes for this long (default 10 s, > heartbeat period)
	WriteTimeout    time.Duration // per-write deadline (default 2 s)
}

func (o *Options) withDefaults() {
	if o.Port == 0 {
		o.Port = 8089
	}
	if o.InitialBackoff == 0 {
		o.InitialBackoff = time.Second
	}
	if o.MaxBackoff == 0 {
		o.MaxBackoff = 30 * time.Second
	}
	if o.ReadIdleTimeout == 0 {
		o.ReadIdleTimeout = 10 * time.Second
	}
	if o.WriteTimeout == 0 {
		o.WriteTimeout = 2 * time.Second
	}
}

// Client is created by Run() and exposes Send() for outbound commands.
type Client struct {
	opts Options
	core *state.Core

	mu   sync.Mutex
	conn net.Conn
}

// Run dials the tuner-controller and maintains the connection until ctx
// is cancelled. Reconnects with exponential backoff (initial → max) on
// any read/write/dial error. Updates core's link status alongside.
func Run(ctx context.Context, core *state.Core, opts Options) *Client {
	opts.withDefaults()
	c := &Client{opts: opts, core: core}
	go c.loop(ctx)
	return c
}

func (c *Client) loop(ctx context.Context) {
	addr := net.JoinHostPort(c.opts.Host, strconv.Itoa(c.opts.Port))
	slog.Info("tunerclient starting", "addr", addr)
	backoff := c.opts.InitialBackoff
	for {
		if ctx.Err() != nil {
			c.core.SetLink(state.LinkDisconnected)
			return
		}
		c.core.SetLink(state.LinkConnecting)
		conn, err := net.DialTimeout("tcp", addr, 5*time.Second)
		if err != nil {
			slog.Debug("tunerclient dial failed", "addr", addr, "err", err, "backoff", backoff)
			c.core.SetLink(state.LinkDisconnected)
			select {
			case <-ctx.Done():
				return
			case <-time.After(backoff):
			}
			backoff = next(backoff, c.opts.MaxBackoff)
			continue
		}
		slog.Info("tunerclient connected", "addr", addr)
		c.core.SetLink(state.LinkConnected)
		c.setConn(conn)
		backoff = c.opts.InitialBackoff
		c.read(ctx, conn)
		c.setConn(nil)
		_ = conn.Close()
		c.core.SetLink(state.LinkDisconnected)
		if ctx.Err() != nil {
			return
		}
	}
}

// read pumps lines from conn until ctx is done or the connection drops.
func (c *Client) read(ctx context.Context, conn net.Conn) {
	scanner := bufio.NewScanner(conn)
	// 128 KB buffer cap is overkill (PROTOCOL.md limit is 64 KB), but
	// safe and well within the master's RAM budget.
	scanner.Buffer(make([]byte, 8*1024), 128*1024)

	// Stop the read loop if ctx is cancelled — closing the conn unblocks Scan().
	go func() {
		<-ctx.Done()
		_ = conn.Close()
	}()

	for scanner.Scan() {
		_ = conn.SetReadDeadline(time.Now().Add(c.opts.ReadIdleTimeout))
		line := scanner.Bytes()
		if len(line) == 0 {
			continue
		}
		c.handleFrame(line)
	}
	if err := scanner.Err(); err != nil && !errors.Is(err, net.ErrClosed) {
		slog.Debug("tunerclient read ended", "err", err)
	}
}

// handleFrame routes one inbound JSON object into the state core.
func (c *Client) handleFrame(line []byte) {
	// Cheap discriminator-only peek so we don't decode the entire payload
	// to the wrong type. Almost all frames are state or heartbeat.
	var head struct {
		Type protocol.FrameType `json:"type"`
	}
	if err := json.Unmarshal(line, &head); err != nil {
		slog.Debug("tunerclient parse type failed", "err", err)
		return
	}
	switch head.Type {
	case protocol.TypeState:
		var f protocol.State
		if err := json.Unmarshal(line, &f); err != nil {
			slog.Debug("tunerclient state decode", "err", err)
			return
		}
		c.core.UpdateState(f.Data)
	case protocol.TypeTelemetry:
		var f protocol.Telemetry
		if err := json.Unmarshal(line, &f); err != nil {
			slog.Debug("tunerclient telemetry decode", "err", err)
			return
		}
		c.core.UpdateTelemetry(f.Data)
	case protocol.TypeHeartbeat:
		// Liveness only; nothing to record.
	case protocol.TypeStatus:
		var f protocol.Status
		if err := json.Unmarshal(line, &f); err == nil {
			slog.Info("controller status", "level", f.Level, "code", f.Code, "msg", f.Msg)
		}
	case protocol.TypeAck:
		// Acks for our outbound commands. We don't track them yet — the
		// hub forwards verbs blindly; M1b interim.
	default:
		slog.Debug("tunerclient ignored frame", "type", head.Type)
	}
}

// Send writes one Command frame to the connected controller. Returns
// an error if the link is currently down or the write fails. Caller
// should treat a Send error as advisory: the link is being repaired by
// the loop, and the user can retry.
func (c *Client) Send(cmd protocol.Command) error {
	c.mu.Lock()
	conn := c.conn
	c.mu.Unlock()
	if conn == nil {
		return errors.New("controller not connected")
	}
	cmd.Type = protocol.TypeCommand
	raw, err := json.Marshal(cmd)
	if err != nil {
		return fmt.Errorf("marshal command: %w", err)
	}
	raw = append(raw, '\n')
	_ = conn.SetWriteDeadline(time.Now().Add(c.opts.WriteTimeout))
	_, err = conn.Write(raw)
	return err
}

func (c *Client) setConn(conn net.Conn) {
	c.mu.Lock()
	c.conn = conn
	c.mu.Unlock()
}

func next(cur, max time.Duration) time.Duration {
	n := cur * 2
	if n > max {
		return max
	}
	return n
}

package tunerclient

import (
	"context"
	"encoding/json"
	"net"
	"testing"
	"time"

	"github.com/VU3ESV/Automatic-Antenna-Tuner/master/tuner-master/internal/protocol"
	"github.com/VU3ESV/Automatic-Antenna-Tuner/master/tuner-master/internal/state"
)

// Stands up a fake "tuner controller" on a local TCP port, runs the
// client against it, and asserts the state.Core is updated.
func TestRun_DecodesStateFrameIntoCore(t *testing.T) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	defer ln.Close()
	host, port := splitHostPort(t, ln.Addr().String())

	core := state.New()
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	Run(ctx, core, Options{Host: host, Port: port, InitialBackoff: 50 * time.Millisecond})

	conn, err := ln.Accept()
	if err != nil {
		t.Fatalf("accept: %v", err)
	}
	defer conn.Close()

	frame := protocol.State{
		Envelope: protocol.Envelope{Type: protocol.TypeState, Seq: 1, TS: time.Now().UTC()},
		Data: protocol.StateData{
			LSteps: 12345, CSteps: 6789,
			LEnc: 12340, CEnc: 6790,
			Side:   protocol.SideLoZ,
			Bypass: false,
			Moving: false,
			Homed:  true,
		},
	}
	raw, _ := json.Marshal(frame)
	raw = append(raw, '\n')
	if _, err := conn.Write(raw); err != nil {
		t.Fatalf("write: %v", err)
	}

	// Wait for core to receive the update.
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		snap := core.Snapshot()
		if snap.State != nil && snap.State.LSteps == 12345 && snap.State.Side == protocol.SideLoZ {
			return // OK
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatalf("core never received the state frame: %+v", core.Snapshot().State)
}

func TestRun_ReconnectsAfterDrop(t *testing.T) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	defer ln.Close()
	host, port := splitHostPort(t, ln.Addr().String())

	core := state.New()
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	Run(ctx, core, Options{Host: host, Port: port, InitialBackoff: 20 * time.Millisecond, MaxBackoff: 100 * time.Millisecond})

	// First connect.
	conn1, err := ln.Accept()
	if err != nil {
		t.Fatalf("accept #1: %v", err)
	}
	// Drop immediately to force a reconnect attempt.
	conn1.Close()

	// Expect a second connection.
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		_ = ln.(*net.TCPListener).SetDeadline(time.Now().Add(100 * time.Millisecond))
		conn2, err := ln.Accept()
		if err == nil {
			conn2.Close()
			return // OK — reconnect happened
		}
	}
	t.Fatal("reconnect never happened")
}

func TestSend_WritesCommandLine(t *testing.T) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	defer ln.Close()
	host, port := splitHostPort(t, ln.Addr().String())

	core := state.New()
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	c := Run(ctx, core, Options{Host: host, Port: port, InitialBackoff: 20 * time.Millisecond})

	conn, err := ln.Accept()
	if err != nil {
		t.Fatalf("accept: %v", err)
	}
	defer conn.Close()

	// Give the client a tick to record the conn pointer.
	time.Sleep(50 * time.Millisecond)

	if err := c.Send(protocol.Command{ID: "test-1", Action: "noop"}); err != nil {
		t.Fatalf("Send: %v", err)
	}

	_ = conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	buf := make([]byte, 256)
	n, err := conn.Read(buf)
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	got := string(buf[:n])
	if !containsAll(got, `"type":"command"`, `"id":"test-1"`, `"action":"noop"`) {
		t.Errorf("unexpected wire: %s", got)
	}
}

func splitHostPort(t *testing.T, addr string) (string, int) {
	t.Helper()
	host, portStr, err := net.SplitHostPort(addr)
	if err != nil {
		t.Fatalf("SplitHostPort: %v", err)
	}
	var port int
	_, err = fmtSscan(portStr, &port)
	if err != nil {
		t.Fatalf("port parse: %v", err)
	}
	return host, port
}

// Avoid pulling in fmt at file scope just for one Sscan in tests.
func fmtSscan(s string, p *int) (int, error) {
	var n int
	for _, r := range s {
		if r < '0' || r > '9' {
			return 0, errInvalidPort
		}
		n = n*10 + int(r-'0')
	}
	*p = n
	return 1, nil
}

func containsAll(haystack string, needles ...string) bool {
	for _, n := range needles {
		if !contains(haystack, n) {
			return false
		}
	}
	return true
}

func contains(s, sub string) bool {
	for i := 0; i+len(sub) <= len(s); i++ {
		if s[i:i+len(sub)] == sub {
			return true
		}
	}
	return false
}

var errInvalidPort = &portError{msg: "invalid port"}

type portError struct{ msg string }

func (e *portError) Error() string { return e.msg }

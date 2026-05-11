package state

import (
	"sync"
	"testing"
	"time"

	"github.com/VU3ESV/Automatic-Antenna-Tuner/master/tuner-master/internal/protocol"
)

func TestNew_StartsInBypassAndHiZ(t *testing.T) {
	c := New()
	snap := c.Snapshot()
	if snap.ControllerLink != LinkDisconnected {
		t.Errorf("ControllerLink = %q, want disconnected", snap.ControllerLink)
	}
	if snap.State == nil {
		t.Fatal("State is nil; should default-construct")
	}
	if !snap.State.Bypass {
		t.Error("Bypass = false; invariant #2 (bypass on power-up) violated")
	}
	if snap.State.Side != protocol.SideHiZ {
		t.Errorf("Side = %q, want hi_z", snap.State.Side)
	}
}

func TestUpdateTelemetry_BroadcastsToSubscribers(t *testing.T) {
	c := New()
	ch := make(chan Event, 4)
	unsub := c.Subscribe(ch)
	defer unsub()

	c.UpdateTelemetry(protocol.TelemetryData{FwdW: 5.0, Mode: protocol.ModeTune})

	select {
	case ev := <-ch:
		if ev.Kind != protocol.TypeTelemetry {
			t.Errorf("Kind = %q, want telemetry", ev.Kind)
		}
		if ev.Snap.Telemetry == nil || ev.Snap.Telemetry.FwdW != 5.0 {
			t.Errorf("Telemetry not propagated: %+v", ev.Snap.Telemetry)
		}
	case <-time.After(time.Second):
		t.Fatal("no event delivered within 1s")
	}
}

func TestSetLink_DoesNotBroadcastIfUnchanged(t *testing.T) {
	c := New()
	ch := make(chan Event, 4)
	unsub := c.Subscribe(ch)
	defer unsub()

	c.SetLink(LinkDisconnected) // already disconnected
	select {
	case ev := <-ch:
		t.Errorf("unexpected event for no-op link change: %+v", ev)
	case <-time.After(50 * time.Millisecond):
		// ok
	}

	c.SetLink(LinkConnected)
	select {
	case ev := <-ch:
		if ev.Snap.ControllerLink != LinkConnected {
			t.Errorf("ControllerLink = %q, want connected", ev.Snap.ControllerLink)
		}
	case <-time.After(time.Second):
		t.Fatal("expected event for link change")
	}
}

func TestSubscribe_Concurrent(t *testing.T) {
	c := New()
	const n = 8
	var wg sync.WaitGroup
	wg.Add(n)
	for i := 0; i < n; i++ {
		go func() {
			defer wg.Done()
			ch := make(chan Event, 16)
			unsub := c.Subscribe(ch)
			defer unsub()
			c.UpdateTelemetry(protocol.TelemetryData{FwdW: 1.0})
			<-ch
		}()
	}
	wg.Wait()
}

func TestUnsubscribe_ClosesChannel(t *testing.T) {
	c := New()
	ch := make(chan Event, 4)
	unsub := c.Subscribe(ch)
	unsub()
	if _, ok := <-ch; ok {
		t.Error("channel should be closed after unsubscribe")
	}
}

func TestBroadcast_DropsToSlowSubscriber(t *testing.T) {
	c := New()
	slow := make(chan Event) // unbuffered: any send w/o receiver drops
	unsub := c.Subscribe(slow)
	defer unsub()

	// Should not block even though `slow` has no reader.
	done := make(chan struct{})
	go func() {
		c.UpdateTelemetry(protocol.TelemetryData{FwdW: 1})
		close(done)
	}()
	select {
	case <-done:
		// ok
	case <-time.After(time.Second):
		t.Fatal("broadcast blocked on slow subscriber")
	}
}

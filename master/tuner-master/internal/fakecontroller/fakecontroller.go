// Package fakecontroller is a placeholder source for the master's
// state.Core, used until the real tuner-controller firmware has its
// Ethernet + WS server (mid-M1 → M2). It generates synthetic
// telemetry that wobbles around a configurable centre so the web UI
// has live data to render during master-only development.
//
// Replace with the real WS client to the tuner-controller as soon as
// the firmware exposes its `/ws` endpoint.
package fakecontroller

import (
	"context"
	"log/slog"
	"math"
	"math/rand"
	"time"

	"github.com/VU3ESV/Automatic-Antenna-Tuner/master/tuner-master/internal/protocol"
	"github.com/VU3ESV/Automatic-Antenna-Tuner/master/tuner-master/internal/state"
)

// Run pushes synthetic frames into core until ctx is cancelled.
func Run(ctx context.Context, core *state.Core) {
	slog.Info("fakecontroller starting (no real controller yet)")
	core.SetLink(state.LinkConnecting)
	time.Sleep(500 * time.Millisecond)
	core.SetLink(state.LinkConnected)

	// Initial state (post-bypass-on-power-up + a plausible memory
	// recall to a 20 m slot).
	core.UpdateState(protocol.StateData{
		LSteps: 18432, CSteps: 9216,
		LEnc: 18434, CEnc: 9214,
		Side:   protocol.SideHiZ,
		Bypass: false,
		Homed:  true,
	})

	tickTel := time.NewTicker(100 * time.Millisecond) // 10 Hz live data
	tickQRG := time.NewTicker(2 * time.Second)
	defer tickTel.Stop()
	defer tickQRG.Stop()

	rng := rand.New(rand.NewSource(time.Now().UnixNano()))
	t0 := time.Now()

	for {
		select {
		case <-ctx.Done():
			core.SetLink(state.LinkDisconnected)
			slog.Info("fakecontroller stopped")
			return
		case <-tickTel.C:
			t := time.Since(t0).Seconds()
			swr := 1.10 + 0.03*math.Sin(t/3) + rng.Float64()*0.005
			fwd := 5.0 + math.Sin(t)*0.2
			rev := fwd * math.Pow((swr-1)/(swr+1), 2)
			mode := protocol.ModeRX
			if fwd > 1.0 {
				mode = protocol.ModeTune
			}
			swrPtr := swr
			core.UpdateTelemetry(protocol.TelemetryData{
				FwdW:   fwd,
				RevW:   rev,
				SWR:    &swrPtr,
				ZMag:   55 + 2*math.Sin(t/2),
				ZPhase: -8 + 1.5*math.Cos(t/2),
				R:      54 + math.Cos(t/2),
				X:      -8 + 1.2*math.Sin(t/2),
				Mode:   mode,
			})
		case <-tickQRG.C:
			core.UpdateQRG(state.QRGData{
				FreqHz: 14175000,
				Mode:   "USB",
				Rig:    "fake",
				Stale:  false,
			})
		}
	}
}

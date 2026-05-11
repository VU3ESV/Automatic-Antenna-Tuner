// tuner-master — M1b scaffold.
//
// Serves the embedded web UI plus a /ws endpoint that streams state +
// telemetry to browser/touch clients (PROTOCOL.md). Connects to the
// real tuner-controller via internal/tunerclient over the M1b interim
// TCP transport (PROTOCOL.md §1.0). The fakecontroller source is kept
// behind a flag for development without hardware.
//
// See docs/PLAN.md M1.

package main

import (
	"context"
	"embed"
	"encoding/json"
	"errors"
	"flag"
	"io/fs"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"sync"
	"syscall"
	"time"

	"github.com/BurntSushi/toml"

	"github.com/VU3ESV/Automatic-Antenna-Tuner/master/tuner-master/internal/fakecontroller"
	"github.com/VU3ESV/Automatic-Antenna-Tuner/master/tuner-master/internal/hub"
	"github.com/VU3ESV/Automatic-Antenna-Tuner/master/tuner-master/internal/state"
	"github.com/VU3ESV/Automatic-Antenna-Tuner/master/tuner-master/internal/tunerclient"
)

//go:embed internal/web/static
var webFS embed.FS

type Config struct {
	Server struct {
		Listen      string `toml:"listen"`
		HeartbeatMs int    `toml:"heartbeat_ms"`
	} `toml:"server"`
	Tuner struct {
		Host        string `toml:"host"`
		Port        int    `toml:"port"`
		ReconnectMs int    `toml:"reconnect_ms"`
	} `toml:"tuner"`
}

func loadConfig(path string) (Config, error) {
	var c Config
	_, err := toml.DecodeFile(path, &c)
	return c, err
}

func main() {
	cfgPath := flag.String("config", "deploy/config.example.toml", "path to TOML config file")
	verbose := flag.Bool("v", false, "enable debug logging")
	fakeCtrl := flag.Bool("fake-controller", false, "use the synthetic controller source instead of dialing the real controller (M1 dev aid)")
	flag.Parse()

	level := slog.LevelError
	if *verbose {
		level = slog.LevelDebug
	}
	slog.SetDefault(slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: level})))

	cfg, err := loadConfig(*cfgPath)
	if err != nil {
		slog.Error("config load failed", "path", *cfgPath, "err", err)
		os.Exit(1)
	}
	slog.Info("config loaded",
		"listen", cfg.Server.Listen,
		"tuner_host", cfg.Tuner.Host,
		"tuner_port", cfg.Tuner.Port,
		"heartbeat_ms", cfg.Server.HeartbeatMs,
		"fake_controller", *fakeCtrl,
	)

	heartbeat := time.Duration(cfg.Server.HeartbeatMs) * time.Millisecond
	if heartbeat <= 0 {
		heartbeat = 2 * time.Second
	}

	core := state.New()
	wsHub := hub.New(core, hub.NoopHandler, heartbeat)

	mux := http.NewServeMux()

	staticFS, err := fs.Sub(webFS, "internal/web/static")
	if err != nil {
		slog.Error("embed fs", "err", err)
		os.Exit(1)
	}
	mux.Handle("/", http.FileServer(http.FS(staticFS)))

	mux.HandleFunc("/healthz", func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write([]byte("ok\n"))
	})

	mux.HandleFunc("/api/config", func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		snap := core.Snapshot()
		_ = json.NewEncoder(w).Encode(map[string]any{
			"phase":      "M1b",
			"controller": snap.ControllerLink,
			"tuner_host": cfg.Tuner.Host,
			"tuner_port": cfg.Tuner.Port,
			"source":     map[bool]string{true: "fakecontroller", false: "tunerclient"}[*fakeCtrl],
		})
	})

	mux.HandleFunc("/ws", wsHub.ServeWS)

	srv := &http.Server{
		Addr:              cfg.Server.Listen,
		Handler:           mux,
		ReadHeaderTimeout: 5 * time.Second,
	}

	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer cancel()

	var wg sync.WaitGroup

	// Choose exactly one controller source.
	if *fakeCtrl {
		wg.Add(1)
		go func() {
			defer wg.Done()
			fakecontroller.Run(ctx, core)
		}()
	} else {
		// Real controller: dial via TCP per PROTOCOL.md §1.0.
		tunerclient.Run(ctx, core, tunerclient.Options{
			Host:           cfg.Tuner.Host,
			Port:           cfg.Tuner.Port,
			InitialBackoff: time.Duration(cfg.Tuner.ReconnectMs) * time.Millisecond,
		})
	}

	wg.Add(1)
	go func() {
		defer wg.Done()
		slog.Info("http listening", "addr", cfg.Server.Listen)
		if err := srv.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
			slog.Error("http serve", "err", err)
		}
	}()

	<-ctx.Done()
	slog.Info("shutting down")

	shutdownCtx, shutdownCancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer shutdownCancel()
	_ = srv.Shutdown(shutdownCtx)
	wg.Wait()
	slog.Info("shutdown complete")
}

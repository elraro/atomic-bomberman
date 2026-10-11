// The master server of Atomic Bomberman's online play: where games are found.
// See docs/specifications/online-play.md (section 1) and master/README.md.
package main

import (
	"context"
	"flag"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"github.com/elraro/atomic-bomberman/master/internal/api"
	"github.com/elraro/atomic-bomberman/master/internal/store"
)

func env(name, fallback string) string {
	if v := os.Getenv(name); v != "" {
		return v
	}
	return fallback
}

func main() {
	listen := flag.String("listen", env("MASTER_LISTEN", ":8443"), "address to listen on")
	cert := flag.String("tls-cert", env("MASTER_TLS_CERT", ""), "TLS certificate file (PEM); with -tls-key: HTTPS")
	key := flag.String("tls-key", env("MASTER_TLS_KEY", ""), "TLS key file (PEM)")
	database := flag.String("database", env("MASTER_DATABASE_URL", ""), "PostgreSQL URL; empty: everything in memory, lost at a restart")
	status := flag.String("status", env("MASTER_STATUS", "ok"), "ok or maintenance")
	message := flag.String("message", env("MASTER_MESSAGE", ""), "message of the day")
	minVersion := flag.Int("min-version", 0, "lowest game protocol version let in")
	turnSecret := flag.String("turn-secret", env("MASTER_TURN_SECRET", ""), "the secret shared with coturn (use-auth-secret)")
	turnURLs := flag.String("turn-urls", env("MASTER_TURN_URLS", ""), "comma-separated TURN URLs handed to games")
	configFile := flag.String("config-file", env("MASTER_CONFIG_FILE", ""), "a signed configuration document to serve at /config.json (it may be served from anywhere else just as well)")
	trustProxy := flag.Bool("trust-proxy", env("MASTER_TRUST_PROXY", "") == "1", "take client addresses from X-Forwarded-For (only behind your own reverse proxy)")
	flag.Parse()

	log := slog.New(slog.NewTextHandler(os.Stderr, nil))
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	var st store.Store
	if *database == "" {
		log.Warn("no database given: sessions and lobbies are kept in memory only")
		st = store.NewMemory()
	} else {
		var err error
		if st, err = store.NewPostgres(ctx, *database); err != nil {
			log.Error("cannot use the database", "error", err)
			os.Exit(1)
		}
	}
	defer st.Close()

	cfg := api.Config{Status: *status, Message: *message, MinVersion: *minVersion, TurnSecret: *turnSecret, TrustProxy: *trustProxy, Log: log}
	if *turnURLs != "" {
		cfg.TurnURLs = strings.Split(*turnURLs, ",")
	}
	handler := api.New(cfg, st)
	mux := http.NewServeMux()
	mux.Handle("/", handler)
	if *configFile != "" {
		// Read anew for every request, so that a newly signed document is served at once.
		mux.HandleFunc("GET /config.json", func(w http.ResponseWriter, _ *http.Request) {
			document, err := os.ReadFile(*configFile)
			if err != nil {
				http.Error(w, "no configuration", http.StatusNotFound)
				return
			}
			w.Header().Set("Content-Type", "application/json")
			w.Header().Set("Cache-Control", "no-store")
			_, _ = w.Write(document)
		})
	}
	go func() {
		tick := time.NewTicker(5 * time.Second)
		defer tick.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-tick.C:
				handler.Housekeeping(ctx)
			}
		}
	}()

	srv := &http.Server{
		Addr:              *listen,
		Handler:           mux,
		ReadHeaderTimeout: 10 * time.Second,
		ReadTimeout:       20 * time.Second,
		WriteTimeout:      40 * time.Second, // above the longest long poll
		IdleTimeout:       2 * time.Minute,
		MaxHeaderBytes:    16 << 10,
	}
	go func() {
		<-ctx.Done()
		shut, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = srv.Shutdown(shut)
	}()
	var err error
	if *cert != "" && *key != "" {
		log.Info("master listening", "address", *listen, "tls", true)
		err = srv.ListenAndServeTLS(*cert, *key)
	} else {
		log.Warn("no TLS certificate given: plain HTTP; put a TLS-terminating proxy in front, or pass -tls-cert and -tls-key")
		log.Info("master listening", "address", *listen, "tls", false)
		err = srv.ListenAndServe()
	}
	if err != nil && err != http.ErrServerClosed {
		log.Error("server stopped", "error", err)
		os.Exit(1)
	}
}

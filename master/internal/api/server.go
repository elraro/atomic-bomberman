// Package api is the master server's HTTP interface (docs/specifications/
// online-play.md, section 1): guest sessions, lobbies found by list or by code,
// the passing of connection details between a host and those joining, and
// short-lived relay credentials. It never carries a game and decides nothing
// about one.
package api

import (
	"context"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha1"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"math/big"
	"net"
	"net/http"
	"strconv"
	"strings"
	"time"
	"unicode"
	"unicode/utf8"

	"github.com/elraro/atomic-bomberman/master/internal/store"
)

// Config is what the operator sets.
type Config struct {
	Status      string   // "ok" or "maintenance"
	Message     string   // message of the day
	MinVersion  int      // lowest game protocol let in
	TurnSecret  string   // shared with coturn (use-auth-secret); empty: no relay credentials
	TurnURLs    []string // e.g. "turn:relay.example:3478?transport=udp"
	TurnTTL     time.Duration
	TrustProxy  bool // take the client's address from X-Forwarded-For (behind a reverse proxy only)
	LobbyTTL    time.Duration
	SessionTTL  time.Duration
	MaxLobbies  int
	Now         func() time.Time // for tests
	Log         *slog.Logger
	WritePerMin int // write calls per address per minute
	CodePerMin  int // code look-ups per address per minute
}

// Server is an http.Handler.
type Server struct {
	cfg    Config
	store  store.Store
	mux    *http.ServeMux
	writes *limiter
	codes  *limiter
}

const (
	maxBody      = 16 << 10
	maxName      = 32
	maxSignal    = 4 << 10
	pageSize     = 50
	maxLongPoll  = 25 * time.Second
	codeAttempts = 20
)

// New fills in defaults for whatever the config leaves at zero.
func New(cfg Config, st store.Store) *Server {
	if cfg.Status == "" {
		cfg.Status = "ok"
	}
	if cfg.TurnTTL == 0 {
		cfg.TurnTTL = time.Hour
	}
	if cfg.LobbyTTL == 0 {
		cfg.LobbyTTL = 30 * time.Second
	}
	if cfg.SessionTTL == 0 {
		cfg.SessionTTL = 24 * time.Hour
	}
	if cfg.MaxLobbies == 0 {
		cfg.MaxLobbies = 5000
	}
	if cfg.Now == nil {
		cfg.Now = time.Now
	}
	if cfg.Log == nil {
		cfg.Log = slog.Default()
	}
	if cfg.WritePerMin == 0 {
		cfg.WritePerMin = 120
	}
	if cfg.CodePerMin == 0 {
		cfg.CodePerMin = 10
	}
	s := &Server{cfg: cfg, store: st, mux: http.NewServeMux(), writes: newLimiter(cfg.WritePerMin, cfg.WritePerMin/4+1), codes: newLimiter(cfg.CodePerMin, cfg.CodePerMin)}
	s.mux.HandleFunc("GET /v1/status", s.status)
	s.mux.HandleFunc("POST /v1/session", s.limited(s.createSession))
	s.mux.HandleFunc("POST /v1/relay-credentials", s.limited(s.withSession(s.relayCredentials)))
	s.mux.HandleFunc("POST /v1/lobbies", s.limited(s.withSession(s.createLobby)))
	s.mux.HandleFunc("GET /v1/lobbies", s.listLobbies)
	s.mux.HandleFunc("POST /v1/lobbies/{id}/join", s.limited(s.withSession(s.joinLobby)))
	s.mux.HandleFunc("POST /v1/lobbies/{id}/leave", s.limited(s.withSession(s.leaveLobby)))
	s.mux.HandleFunc("PUT /v1/lobbies/{id}", s.limited(s.withHost(s.updateLobby)))
	s.mux.HandleFunc("DELETE /v1/lobbies/{id}", s.limited(s.withHost(s.deleteLobby)))
	s.mux.HandleFunc("POST /v1/lobbies/{id}/signal", s.limited(s.postSignal))
	// ".../by-code/{code}" and ".../{id}/signal" have the same shape; one handler tells them apart.
	s.mux.HandleFunc("GET /v1/lobbies/{id}/{what}", func(w http.ResponseWriter, r *http.Request) {
		switch {
		case r.PathValue("id") == "by-code":
			s.lobbyByCode(w, r)
		case r.PathValue("what") == "signal":
			s.getSignal(w, r)
		default:
			http.NotFound(w, r)
		}
	})
	return s
}

func (s *Server) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	r.Body = http.MaxBytesReader(w, r.Body, maxBody)
	s.mux.ServeHTTP(w, r)
}

// Housekeeping removes what has gone quiet. Call it every few seconds.
func (s *Server) Housekeeping(ctx context.Context) {
	now := s.cfg.Now()
	if err := s.store.Expire(ctx, now.Add(-s.cfg.LobbyTTL), now.Add(-s.cfg.SessionTTL)); err != nil {
		s.cfg.Log.Error("expire", "error", err)
	}
	s.writes.sweep(now)
	s.codes.sweep(now)
}

// --- helpers ---

type apiError struct {
	Error string `json:"error"`
}

func reply(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

func fail(w http.ResponseWriter, status int, message string) { reply(w, status, apiError{message}) }

func (s *Server) internal(w http.ResponseWriter, err error) {
	s.cfg.Log.Error("store", "error", err)
	fail(w, http.StatusInternalServerError, "The server could not do that just now")
}

func readJSON(r *http.Request, v any) bool {
	dec := json.NewDecoder(r.Body)
	return dec.Decode(v) == nil
}

func (s *Server) clientIP(r *http.Request) string {
	if s.cfg.TrustProxy {
		if fwd := r.Header.Get("X-Forwarded-For"); fwd != "" {
			first := strings.TrimSpace(strings.Split(fwd, ",")[0])
			if net.ParseIP(first) != nil {
				return first
			}
		}
	}
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		return r.RemoteAddr
	}
	return host
}

// Random values come from the system's cryptographic source, never from math/rand.
func randomHex(bytes int) string {
	raw := make([]byte, bytes)
	if _, err := rand.Read(raw); err != nil {
		panic(err) // the system has no randomness: nothing safe can be done
	}
	return hex.EncodeToString(raw)
}

func randomCode() string {
	n, err := rand.Int(rand.Reader, big.NewInt(1000000))
	if err != nil {
		panic(err)
	}
	return fmt.Sprintf("%06d", n.Int64())
}

func hashToken(token string) string {
	sum := sha256.Sum256([]byte(token))
	return hex.EncodeToString(sum[:])
}

// cleanText keeps printable characters, trims, and cuts at max bytes on a character boundary.
func cleanText(in string, max int) string {
	var b strings.Builder
	for _, r := range in {
		if r == utf8.RuneError || !unicode.IsPrint(r) {
			continue
		}
		if b.Len()+utf8.RuneLen(r) > max {
			break
		}
		b.WriteRune(r)
	}
	return strings.TrimSpace(b.String())
}

func bearer(r *http.Request) string {
	const prefix = "Bearer "
	h := r.Header.Get("Authorization")
	if !strings.HasPrefix(h, prefix) {
		return ""
	}
	return h[len(prefix):]
}

func (s *Server) limited(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if !s.writes.allow(s.clientIP(r), s.cfg.Now()) {
			fail(w, http.StatusTooManyRequests, "Too many requests; try again in a moment")
			return
		}
		next(w, r)
	}
}

type sessionKey struct{}
type lobbyKey struct{}

func (s *Server) session(r *http.Request) (store.Session, error) {
	token := bearer(r)
	if token == "" {
		return store.Session{}, store.ErrNotFound
	}
	return s.store.SessionByTokenHash(r.Context(), hashToken(token), s.cfg.Now())
}

func (s *Server) withSession(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		sess, err := s.session(r)
		if errors.Is(err, store.ErrNotFound) {
			fail(w, http.StatusUnauthorized, "No session; start one first")
			return
		}
		if err != nil {
			s.internal(w, err)
			return
		}
		next(w, r.WithContext(context.WithValue(r.Context(), sessionKey{}, sess)))
	}
}

// hostOf returns the lobby if the request carries its host token.
func (s *Server) hostOf(r *http.Request) (store.Lobby, bool, error) {
	l, err := s.store.Lobby(r.Context(), r.PathValue("id"))
	if err != nil {
		return store.Lobby{}, false, err
	}
	given := hashToken(r.Header.Get("X-Host-Token"))
	return l, subtle.ConstantTimeCompare([]byte(given), []byte(l.HostTokenHash)) == 1, nil
}

func (s *Server) withHost(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		l, isHost, err := s.hostOf(r)
		if errors.Is(err, store.ErrNotFound) || (err == nil && !isHost) {
			// The same answer whether the lobby is gone or the token is wrong.
			fail(w, http.StatusNotFound, "No such lobby")
			return
		}
		if err != nil {
			s.internal(w, err)
			return
		}
		next(w, r.WithContext(context.WithValue(r.Context(), lobbyKey{}, l)))
	}
}

// --- what a lobby looks like from outside ---

type lobbyView struct {
	ID        string `json:"id"`
	Name      string `json:"name"`
	Players   int    `json:"players"`
	Seats     int    `json:"seats"`
	Phase     string `json:"phase"`
	Level     string `json:"level"`
	Version   int    `json:"version"`
	Region    string `json:"region"`
	Certified bool   `json:"certified"`
}

func view(l store.Lobby) lobbyView {
	return lobbyView{l.ID, l.Name, l.Players, l.Seats, l.Phase, l.Level, l.Version, l.Region, l.Certificate != ""}
}

// --- handlers ---

func (s *Server) status(w http.ResponseWriter, _ *http.Request) {
	reply(w, http.StatusOK, map[string]any{"status": s.cfg.Status, "message": s.cfg.Message, "min_version": s.cfg.MinVersion})
}

func (s *Server) createSession(w http.ResponseWriter, r *http.Request) {
	var in struct {
		Name    string `json:"name"`
		Version int    `json:"version"`
	}
	if !readJSON(r, &in) {
		fail(w, http.StatusBadRequest, "Malformed request")
		return
	}
	if s.cfg.Status == "maintenance" {
		fail(w, http.StatusServiceUnavailable, "Down for maintenance: "+s.cfg.Message)
		return
	}
	if in.Version < s.cfg.MinVersion {
		fail(w, http.StatusUpgradeRequired, "This version of the game is too old for online play")
		return
	}
	name := cleanText(in.Name, maxName)
	if name == "" {
		name = "Player"
	}
	now := s.cfg.Now()
	token := randomHex(32)
	sess := store.Session{ID: randomHex(8), TokenHash: hashToken(token), Name: name, Version: in.Version, Created: now, Seen: now}
	if err := s.store.CreateSession(r.Context(), sess); err != nil {
		s.internal(w, err)
		return
	}
	reply(w, http.StatusCreated, map[string]any{"token": token, "id": sess.ID, "name": name})
}

// turnCredentials are the time-limited ones coturn accepts in use-auth-secret mode:
// the user name is "expiry:session", the password HMAC-SHA1 of it under the shared secret.
func (s *Server) turnCredentials(sessionID string) map[string]any {
	if s.cfg.TurnSecret == "" {
		return nil
	}
	username := strconv.FormatInt(s.cfg.Now().Add(s.cfg.TurnTTL).Unix(), 10) + ":" + sessionID
	mac := hmac.New(sha1.New, []byte(s.cfg.TurnSecret))
	mac.Write([]byte(username))
	return map[string]any{
		"username": username,
		"password": base64.StdEncoding.EncodeToString(mac.Sum(nil)),
		"ttl":      int(s.cfg.TurnTTL.Seconds()),
		"urls":     s.cfg.TurnURLs,
	}
}

func (s *Server) relayCredentials(w http.ResponseWriter, r *http.Request) {
	sess := r.Context().Value(sessionKey{}).(store.Session)
	creds := s.turnCredentials(sess.ID)
	if creds == nil {
		fail(w, http.StatusNotFound, "No relay is set up")
		return
	}
	reply(w, http.StatusOK, creds)
}

type lobbyIn struct {
	Name        string   `json:"name"`
	Public      bool     `json:"public"`
	Players     int      `json:"players"`
	Seats       int      `json:"seats"`
	Phase       string   `json:"phase"`
	Level       string   `json:"level"`
	Region      string   `json:"region"`
	IdentityKey string   `json:"identity_key"`
	Certificate string   `json:"certificate"`
	Port        int      `json:"port"`       // where the game server listens, as far as the host knows
	Candidates  []string `json:"candidates"` // further ways to reach it (later: ICE candidates)
}

// apply copies the host's description into the lobby, cleaned and bounded.
func (s *Server) apply(l *store.Lobby, in lobbyIn, ip string) bool {
	if in.Players < 0 || in.Players > 10 || in.Seats < 0 || in.Seats > 10 || in.Port < 0 || in.Port > 65535 || len(in.Candidates) > 16 || len(in.Certificate) > 4096 {
		return false
	}
	if key, err := hex.DecodeString(in.IdentityKey); err != nil || len(key) != 32 {
		return false
	}
	l.Name = cleanText(in.Name, maxName)
	if l.Name == "" {
		l.Name = "A game"
	}
	l.Public = in.Public
	l.Players, l.Seats = in.Players, in.Seats
	l.Phase = cleanText(in.Phase, 16)
	l.Level = cleanText(in.Level, 32)
	l.Region = cleanText(in.Region, 16)
	l.IdentityKey = strings.ToLower(in.IdentityKey)
	l.Certificate = in.Certificate
	// The address the request came from is the first way in; the host may name others.
	l.Candidates = nil
	if in.Port > 0 {
		l.Candidates = append(l.Candidates, net.JoinHostPort(ip, strconv.Itoa(in.Port)))
	}
	for _, c := range in.Candidates {
		if c = cleanText(c, 256); c != "" {
			l.Candidates = append(l.Candidates, c)
		}
	}
	return true
}

func (s *Server) createLobby(w http.ResponseWriter, r *http.Request) {
	sess := r.Context().Value(sessionKey{}).(store.Session)
	var in lobbyIn
	if !readJSON(r, &in) {
		fail(w, http.StatusBadRequest, "Malformed request")
		return
	}
	now := s.cfg.Now()
	hostToken := randomHex(32)
	l := store.Lobby{ID: randomHex(8), HostTokenHash: hashToken(hostToken), HostSession: sess.ID, Version: sess.Version, Created: now, Seen: now}
	if !s.apply(&l, in, s.clientIP(r)) {
		fail(w, http.StatusBadRequest, "Malformed request")
		return
	}
	if _, total, err := s.store.ListPublic(r.Context(), 0, 1); err != nil {
		s.internal(w, err)
		return
	} else if total >= s.cfg.MaxLobbies {
		fail(w, http.StatusServiceUnavailable, "Too many games are open just now")
		return
	}
	// A code nobody else has. With a million codes a clash is rare; a few tries always do.
	for try := 0; ; try++ {
		l.Code = randomCode()
		err := s.store.CreateLobby(r.Context(), l)
		if err == nil {
			break
		}
		if !errors.Is(err, store.ErrCodeTaken) || try >= codeAttempts {
			s.internal(w, err)
			return
		}
	}
	s.cfg.Log.Info("lobby created", "id", l.ID, "public", l.Public, "session", sess.ID)
	reply(w, http.StatusCreated, map[string]any{"id": l.ID, "code": l.Code, "host_token": hostToken})
}

func (s *Server) listLobbies(w http.ResponseWriter, r *http.Request) {
	page, _ := strconv.Atoi(r.URL.Query().Get("page"))
	if page < 0 || page > 10000 {
		page = 0
	}
	lobbies, total, err := s.store.ListPublic(r.Context(), page*pageSize, pageSize)
	if err != nil {
		s.internal(w, err)
		return
	}
	out := make([]lobbyView, 0, len(lobbies))
	for _, l := range lobbies {
		out = append(out, view(l))
	}
	reply(w, http.StatusOK, map[string]any{"lobbies": out, "total": total, "page": page, "page_size": pageSize})
}

func (s *Server) lobbyByCode(w http.ResponseWriter, r *http.Request) {
	// Guessing codes is slowed down for each address; a wrong code and none look the same.
	if !s.codes.allow(s.clientIP(r), s.cfg.Now()) {
		fail(w, http.StatusTooManyRequests, "Too many tries; wait a minute")
		return
	}
	l, err := s.store.LobbyByCode(r.Context(), r.PathValue("what"))
	if errors.Is(err, store.ErrNotFound) {
		fail(w, http.StatusNotFound, "No game with that code")
		return
	}
	if err != nil {
		s.internal(w, err)
		return
	}
	reply(w, http.StatusOK, view(l))
}

func (s *Server) joinLobby(w http.ResponseWriter, r *http.Request) {
	sess := r.Context().Value(sessionKey{}).(store.Session)
	var in struct {
		Code string `json:"code"`
	}
	_ = readJSON(r, &in) // no body is fine for a listed game
	l, err := s.store.Lobby(r.Context(), r.PathValue("id"))
	// A game that is not listed is joined with its code; without it, it does not exist for the asker.
	if errors.Is(err, store.ErrNotFound) || (err == nil && !l.Public && subtle.ConstantTimeCompare([]byte(in.Code), []byte(l.Code)) != 1) {
		if err == nil && !s.codes.allow(s.clientIP(r), s.cfg.Now()) {
			fail(w, http.StatusTooManyRequests, "Too many tries; wait a minute")
			return
		}
		fail(w, http.StatusNotFound, "No such lobby")
		return
	}
	if err != nil {
		s.internal(w, err)
		return
	}
	if l.Version != sess.Version {
		fail(w, http.StatusConflict, "That game is played with another version of the game")
		return
	}
	// The host is told somebody is on the way (it needs the session id to answer with connection details).
	_ = s.store.PushSignal(r.Context(), l.ID, "host", store.Signal{From: sess.ID, Data: "join"})
	reply(w, http.StatusOK, map[string]any{
		"lobby":        view(l),
		"candidates":   l.Candidates,
		"identity_key": l.IdentityKey,
		"certificate":  l.Certificate,
		"relay":        s.turnCredentials(sess.ID),
	})
}

func (s *Server) leaveLobby(w http.ResponseWriter, r *http.Request) {
	sess := r.Context().Value(sessionKey{}).(store.Session)
	_ = s.store.PushSignal(r.Context(), r.PathValue("id"), "host", store.Signal{From: sess.ID, Data: "leave"})
	reply(w, http.StatusOK, map[string]any{})
}

func (s *Server) updateLobby(w http.ResponseWriter, r *http.Request) {
	l := r.Context().Value(lobbyKey{}).(store.Lobby)
	var in lobbyIn
	if !readJSON(r, &in) || !s.apply(&l, in, s.clientIP(r)) {
		fail(w, http.StatusBadRequest, "Malformed request")
		return
	}
	l.Seen = s.cfg.Now()
	if err := s.store.UpdateLobby(r.Context(), l); err != nil {
		if errors.Is(err, store.ErrNotFound) {
			fail(w, http.StatusNotFound, "No such lobby")
			return
		}
		s.internal(w, err)
		return
	}
	reply(w, http.StatusOK, map[string]any{"code": l.Code})
}

func (s *Server) deleteLobby(w http.ResponseWriter, r *http.Request) {
	l := r.Context().Value(lobbyKey{}).(store.Lobby)
	if err := s.store.DeleteLobby(r.Context(), l.ID); err != nil && !errors.Is(err, store.ErrNotFound) {
		s.internal(w, err)
		return
	}
	reply(w, http.StatusOK, map[string]any{})
}

// Who is asking about signals: the host (by its token) or a session.
func (s *Server) signalParty(r *http.Request) (lobbyID, me string, isHost bool, status int) {
	l, host, err := s.hostOf(r)
	if errors.Is(err, store.ErrNotFound) {
		return "", "", false, http.StatusNotFound
	}
	if err != nil {
		return "", "", false, http.StatusInternalServerError
	}
	if host {
		return l.ID, "host", true, 0
	}
	sess, err := s.session(r)
	if err != nil {
		return "", "", false, http.StatusUnauthorized
	}
	return l.ID, sess.ID, false, 0
}

func (s *Server) postSignal(w http.ResponseWriter, r *http.Request) {
	lobbyID, me, isHost, status := s.signalParty(r)
	if status != 0 {
		fail(w, status, "Not allowed")
		return
	}
	var in struct {
		To   string `json:"to"`
		Data string `json:"data"`
	}
	if !readJSON(r, &in) || len(in.Data) > maxSignal || in.To == "" || len(in.To) > 64 {
		fail(w, http.StatusBadRequest, "Malformed request")
		return
	}
	// A player speaks to the host only; the host to any one player.
	if !isHost && in.To != "host" {
		fail(w, http.StatusForbidden, "Not allowed")
		return
	}
	if err := s.store.PushSignal(r.Context(), lobbyID, in.To, store.Signal{From: me, Data: in.Data}); err != nil {
		if errors.Is(err, store.ErrNotFound) {
			fail(w, http.StatusNotFound, "No such lobby")
			return
		}
		s.internal(w, err)
		return
	}
	reply(w, http.StatusOK, map[string]any{})
}

// getSignal waits (long poll) until something is there for the asker, at most `wait` seconds.
func (s *Server) getSignal(w http.ResponseWriter, r *http.Request) {
	lobbyID, me, _, status := s.signalParty(r)
	if status != 0 {
		fail(w, status, "Not allowed")
		return
	}
	wait := time.Duration(0)
	if n, err := strconv.Atoi(r.URL.Query().Get("wait")); err == nil && n > 0 {
		wait = min(time.Duration(n)*time.Second, maxLongPoll)
	}
	deadline := time.Now().Add(wait)
	for {
		signals, err := s.store.PopSignals(r.Context(), lobbyID, me)
		if errors.Is(err, store.ErrNotFound) {
			fail(w, http.StatusNotFound, "No such lobby")
			return
		}
		if err != nil {
			s.internal(w, err)
			return
		}
		if len(signals) > 0 || !time.Now().Before(deadline) {
			if signals == nil {
				signals = []store.Signal{}
			}
			reply(w, http.StatusOK, map[string]any{"signals": signals})
			return
		}
		select {
		case <-r.Context().Done():
			return
		case <-time.After(250 * time.Millisecond):
		}
	}
}

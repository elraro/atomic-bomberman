package api

import (
	"bytes"
	"context"
	"crypto/hmac"
	"crypto/sha1"
	"encoding/base64"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"regexp"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/elraro/atomic-bomberman/master/internal/store"
)

type clock struct {
	mu  sync.Mutex
	now time.Time
}

func (c *clock) Now() time.Time {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.now
}

func (c *clock) add(d time.Duration) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.now = c.now.Add(d)
}

type rig struct {
	t     *testing.T
	srv   *httptest.Server
	api   *Server
	clock *clock
}

// Each test runs against the memory store, and against PostgreSQL too where one is given.
func eachStore(t *testing.T, fn func(t *testing.T, st store.Store)) {
	t.Run("memory", func(t *testing.T) { fn(t, store.NewMemory()) })
	if url := os.Getenv("MASTER_TEST_DATABASE_URL"); url != "" {
		t.Run("postgres", func(t *testing.T) {
			st, err := store.NewPostgres(context.Background(), url)
			if err != nil {
				t.Fatal(err)
			}
			// Left-overs of the test before: everything is older than any lobby time-out.
			_ = st.Expire(context.Background(), time.Now().Add(1000*time.Hour), time.Now().Add(1000*time.Hour))
			fn(t, st)
		})
	}
}

func newRig(t *testing.T, st store.Store, cfg Config) *rig {
	c := &clock{now: time.Now()}
	cfg.Now = c.Now
	if cfg.WritePerMin == 0 {
		cfg.WritePerMin = 100000
	}
	api := New(cfg, st)
	srv := httptest.NewServer(api)
	t.Cleanup(func() {
		srv.Close()
		st.Close()
	})
	return &rig{t: t, srv: srv, api: api, clock: c}
}

type headers map[string]string

func (r *rig) call(method, path string, h headers, body any) (int, map[string]any) {
	r.t.Helper()
	var reader io.Reader
	if s, ok := body.(string); ok {
		reader = strings.NewReader(s)
	} else if body != nil {
		raw, _ := json.Marshal(body)
		reader = bytes.NewReader(raw)
	}
	req, _ := http.NewRequest(method, r.srv.URL+path, reader)
	for k, v := range h {
		req.Header.Set(k, v)
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		r.t.Fatal(err)
	}
	defer resp.Body.Close()
	var out map[string]any
	_ = json.NewDecoder(resp.Body).Decode(&out)
	return resp.StatusCode, out
}

func (r *rig) session(name string, version int) (token, id string) {
	r.t.Helper()
	status, out := r.call("POST", "/v1/session", nil, map[string]any{"name": name, "version": version})
	if status != http.StatusCreated {
		r.t.Fatalf("session: %d %v", status, out)
	}
	return out["token"].(string), out["id"].(string)
}

func auth(token string) headers { return headers{"Authorization": "Bearer " + token} }

const identity = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"

func lobbyBody(name string, public bool) map[string]any {
	return map[string]any{"name": name, "public": public, "players": 1, "seats": 10, "phase": "lobby", "level": "Green Acres", "identity_key": identity, "port": 27410}
}

func (r *rig) lobby(token, name string, public bool) (id, code, hostToken string) {
	r.t.Helper()
	status, out := r.call("POST", "/v1/lobbies", auth(token), lobbyBody(name, public))
	if status != http.StatusCreated {
		r.t.Fatalf("lobby: %d %v", status, out)
	}
	return out["id"].(string), out["code"].(string), out["host_token"].(string)
}

func TestStatusAndSessions(t *testing.T) {
	eachStore(t, func(t *testing.T, st store.Store) {
		r := newRig(t, st, Config{Message: "hello", MinVersion: 8})
		if status, out := r.call("GET", "/v1/status", nil, nil); status != 200 || out["status"] != "ok" || out["message"] != "hello" || out["min_version"] != float64(8) {
			t.Fatalf("status: %d %v", status, out)
		}
		if status, _ := r.call("POST", "/v1/session", nil, map[string]any{"name": "Old", "version": 7}); status != http.StatusUpgradeRequired {
			t.Fatalf("old version let in: %d", status)
		}
		if status, _ := r.call("POST", "/v1/session", nil, "not json"); status != http.StatusBadRequest {
			t.Fatalf("junk: %d", status)
		}
		// A name is cleaned and cut; an empty one gets a default.
		_, out := r.call("POST", "/v1/session", nil, map[string]any{"name": "  Ann\x00\n" + strings.Repeat("x", 100), "version": 8})
		if name := out["name"].(string); !strings.HasPrefix(name, "Ann") || len(name) > 32 || strings.ContainsAny(name, "\x00\n") {
			t.Fatalf("name: %q", name)
		}
		if _, out := r.call("POST", "/v1/session", nil, map[string]any{"version": 8}); out["name"] != "Player" {
			t.Fatalf("default name: %v", out)
		}
		// No session, a made-up one: no lobby.
		if status, _ := r.call("POST", "/v1/lobbies", nil, lobbyBody("x", true)); status != http.StatusUnauthorized {
			t.Fatalf("no session: %d", status)
		}
		if status, _ := r.call("POST", "/v1/lobbies", auth("made-up"), lobbyBody("x", true)); status != http.StatusUnauthorized {
			t.Fatalf("made-up session: %d", status)
		}
		// A body beyond the limit is refused.
		token, _ := r.session("Ann", 8)
		if status, _ := r.call("POST", "/v1/lobbies", auth(token), `{"name":"`+strings.Repeat("a", 20000)+`"}`); status != http.StatusBadRequest {
			t.Fatalf("huge body: %d", status)
		}
	})
	t.Run("maintenance", func(t *testing.T) {
		r := newRig(t, store.NewMemory(), Config{Status: "maintenance", Message: "back at noon"})
		if status, out := r.call("POST", "/v1/session", nil, map[string]any{"name": "Ann", "version": 8}); status != http.StatusServiceUnavailable || !strings.Contains(out["error"].(string), "noon") {
			t.Fatalf("maintenance: %d %v", status, out)
		}
	})
}

func TestLobbies(t *testing.T) {
	eachStore(t, func(t *testing.T, st store.Store) {
		r := newRig(t, st, Config{})
		host, _ := r.session("Host", 8)
		guest, _ := r.session("Guest", 8)
		other, _ := r.session("Other version", 9)

		openID, openCode, openHost := r.lobby(host, "Open game", true)
		hiddenID, hiddenCode, _ := r.lobby(host, "Private game", false)
		if !regexp.MustCompile(`^[0-9]{6}$`).MatchString(openCode) || !regexp.MustCompile(`^[0-9]{6}$`).MatchString(hiddenCode) || openCode == hiddenCode {
			t.Fatalf("codes: %q %q", openCode, hiddenCode)
		}
		// A lobby needs a server identity of the right shape.
		bad := lobbyBody("x", true)
		bad["identity_key"] = "zz"
		if status, _ := r.call("POST", "/v1/lobbies", auth(host), bad); status != http.StatusBadRequest {
			t.Fatalf("bad identity: %d", status)
		}

		// The list shows the public one only, and nothing that helps to get in elsewhere.
		status, out := r.call("GET", "/v1/lobbies", nil, nil)
		list := out["lobbies"].([]any)
		if status != 200 || len(list) != 1 || out["total"] != float64(1) {
			t.Fatalf("list: %d %v", status, out)
		}
		entry := list[0].(map[string]any)
		if entry["id"] != openID || entry["name"] != "Open game" || entry["seats"] != float64(10) || entry["certified"] != false {
			t.Fatalf("entry: %v", entry)
		}
		for _, secret := range []string{"code", "host_token", "candidates"} {
			if _, there := entry[secret]; there {
				t.Fatalf("the list gives away %q", secret)
			}
		}

		// By code: the private one is found; a wrong code and an unknown one answer alike.
		if status, out := r.call("GET", "/v1/lobbies/by-code/"+hiddenCode, nil, nil); status != 200 || out["id"] != hiddenID {
			t.Fatalf("by code: %d %v", status, out)
		}
		wrong := "000000"
		if wrong == hiddenCode || wrong == openCode {
			wrong = "000001"
		}
		s1, o1 := r.call("GET", "/v1/lobbies/by-code/"+wrong, nil, nil)
		s2, o2 := r.call("GET", "/v1/lobbies/by-code/abc", nil, nil)
		if s1 != 404 || s2 != 404 || o1["error"] != o2["error"] {
			t.Fatalf("wrong code: %d %v / %d %v", s1, o1, s2, o2)
		}

		// Joining: the public one as it is; the private one only with its code.
		status, out = r.call("POST", "/v1/lobbies/"+openID+"/join", auth(guest), nil)
		if status != 200 || out["identity_key"] != identity {
			t.Fatalf("join: %d %v", status, out)
		}
		if c := out["candidates"].([]any); len(c) != 1 || c[0] != "127.0.0.1:27410" {
			t.Fatalf("candidates: %v", c)
		}
		if status, _ := r.call("POST", "/v1/lobbies/"+hiddenID+"/join", auth(guest), nil); status != 404 {
			t.Fatalf("private game joined without its code: %d", status)
		}
		if status, _ := r.call("POST", "/v1/lobbies/"+hiddenID+"/join", auth(guest), map[string]any{"code": wrong}); status != 404 {
			t.Fatalf("private game joined with a wrong code: %d", status)
		}
		if status, out := r.call("POST", "/v1/lobbies/"+hiddenID+"/join", auth(guest), map[string]any{"code": hiddenCode}); status != 200 || len(out["candidates"].([]any)) != 1 {
			t.Fatalf("private game with its code: %d %v", status, out)
		}
		if status, _ := r.call("POST", "/v1/lobbies/"+openID+"/join", nil, nil); status != http.StatusUnauthorized {
			t.Fatalf("join without a session: %d", status)
		}
		if status, _ := r.call("POST", "/v1/lobbies/"+openID+"/join", auth(other), nil); status != http.StatusConflict {
			t.Fatalf("join with another version: %d", status)
		}

		// Only the host changes or closes a lobby; a wrong token and a lobby that is not there answer alike.
		update := lobbyBody("Open game", true)
		update["players"] = 3
		update["phase"] = "round"
		update["candidates"] = []string{"203.0.113.9:27410"}
		if status, _ := r.call("PUT", "/v1/lobbies/"+openID, headers{"X-Host-Token": "guess"}, update); status != 404 {
			t.Fatalf("update with a wrong token: %d", status)
		}
		if status, _ := r.call("PUT", "/v1/lobbies/"+openID, headers{"X-Host-Token": openHost}, update); status != 200 {
			t.Fatalf("update: %d", status)
		}
		_, out = r.call("GET", "/v1/lobbies", nil, nil)
		if e := out["lobbies"].([]any)[0].(map[string]any); e["players"] != float64(3) || e["phase"] != "round" {
			t.Fatalf("after update: %v", e)
		}
		_, out = r.call("POST", "/v1/lobbies/"+openID+"/join", auth(guest), nil)
		if c := out["candidates"].([]any); len(c) != 2 || c[1] != "203.0.113.9:27410" {
			t.Fatalf("candidates after update: %v", c)
		}

		// A lobby that says nothing for 30 s is removed; one that keeps saying it is there stays.
		r.clock.add(20 * time.Second)
		r.call("PUT", "/v1/lobbies/"+openID, headers{"X-Host-Token": openHost}, update)
		r.clock.add(20 * time.Second)
		r.api.Housekeeping(context.Background())
		if status, _ := r.call("GET", "/v1/lobbies/by-code/"+hiddenCode, nil, nil); status != 404 {
			t.Fatalf("silent lobby still there: %d", status)
		}
		if status, _ := r.call("GET", "/v1/lobbies/by-code/"+openCode, nil, nil); status != 200 {
			t.Fatalf("live lobby gone: %d", status)
		}
		if status, _ := r.call("DELETE", "/v1/lobbies/"+openID, headers{"X-Host-Token": "guess"}, nil); status != 404 {
			t.Fatalf("close with a wrong token: %d", status)
		}
		if status, _ := r.call("DELETE", "/v1/lobbies/"+openID, headers{"X-Host-Token": openHost}, nil); status != 200 {
			t.Fatalf("close: %d", status)
		}
		if _, out := r.call("GET", "/v1/lobbies", nil, nil); len(out["lobbies"].([]any)) != 0 {
			t.Fatalf("closed lobby listed: %v", out)
		}
	})
}

func TestSignals(t *testing.T) {
	eachStore(t, func(t *testing.T, st store.Store) {
		r := newRig(t, st, Config{})
		host, _ := r.session("Host", 8)
		guest, guestID := r.session("Guest", 8)
		stranger, strangerID := r.session("Stranger", 8)
		id, _, hostToken := r.lobby(host, "Game", true)
		asHost := headers{"X-Host-Token": hostToken}

		// Joining tells the host who is on the way.
		r.call("POST", "/v1/lobbies/"+id+"/join", auth(guest), nil)
		_, out := r.call("GET", "/v1/lobbies/"+id+"/signal", asHost, nil)
		signals := out["signals"].([]any)
		if len(signals) != 1 || signals[0].(map[string]any)["from"] != guestID || signals[0].(map[string]any)["data"] != "join" {
			t.Fatalf("join notice: %v", out)
		}
		// Host to guest, guest to host; each reads its own, once.
		if status, _ := r.call("POST", "/v1/lobbies/"+id+"/signal", asHost, map[string]any{"to": guestID, "data": "candidates of the host"}); status != 200 {
			t.Fatalf("host signal: %d", status)
		}
		if status, _ := r.call("POST", "/v1/lobbies/"+id+"/signal", auth(guest), map[string]any{"to": "host", "data": "candidates of the guest"}); status != 200 {
			t.Fatalf("guest signal: %d", status)
		}
		_, out = r.call("GET", "/v1/lobbies/"+id+"/signal", auth(guest), nil)
		if s := out["signals"].([]any); len(s) != 1 || s[0].(map[string]any)["data"] != "candidates of the host" || s[0].(map[string]any)["from"] != "host" {
			t.Fatalf("guest reads: %v", out)
		}
		_, out = r.call("GET", "/v1/lobbies/"+id+"/signal", asHost, nil)
		if s := out["signals"].([]any); len(s) != 1 || s[0].(map[string]any)["data"] != "candidates of the guest" {
			t.Fatalf("host reads: %v", out)
		}
		if _, out := r.call("GET", "/v1/lobbies/"+id+"/signal", auth(stranger), nil); len(out["signals"].([]any)) != 0 {
			t.Fatalf("a stranger reads somebody's signals: %v", out)
		}
		// A player cannot write to another player, nor anybody without a session or the host token.
		if status, _ := r.call("POST", "/v1/lobbies/"+id+"/signal", auth(guest), map[string]any{"to": strangerID, "data": "x"}); status != http.StatusForbidden {
			t.Fatalf("player to player: %d", status)
		}
		if status, _ := r.call("POST", "/v1/lobbies/"+id+"/signal", nil, map[string]any{"to": "host", "data": "x"}); status != http.StatusUnauthorized {
			t.Fatalf("nobody to host: %d", status)
		}
		if status, _ := r.call("POST", "/v1/lobbies/"+id+"/signal", auth(guest), map[string]any{"to": "host", "data": strings.Repeat("x", 5000)}); status != http.StatusBadRequest {
			t.Fatalf("huge signal: %d", status)
		}
		if status, _ := r.call("GET", "/v1/lobbies/nowhere/signal", auth(guest), nil); status != 404 {
			t.Fatalf("signals of no lobby: %d", status)
		}
		// Waiting: the answer comes when something arrives, not when the wait is over.
		done := make(chan map[string]any, 1)
		go func() {
			_, out := r.call("GET", "/v1/lobbies/"+id+"/signal?wait=20", asHost, nil)
			done <- out
		}()
		time.Sleep(300 * time.Millisecond)
		start := time.Now()
		r.call("POST", "/v1/lobbies/"+id+"/signal", auth(guest), map[string]any{"to": "host", "data": "late"})
		select {
		case out := <-done:
			if s := out["signals"].([]any); len(s) != 1 || s[0].(map[string]any)["data"] != "late" {
				t.Fatalf("long poll: %v", out)
			}
			if time.Since(start) > 3*time.Second {
				t.Fatalf("long poll answered late")
			}
		case <-time.After(10 * time.Second):
			t.Fatal("long poll did not answer")
		}
		// Leaving is told to the host too.
		r.call("POST", "/v1/lobbies/"+id+"/leave", auth(guest), nil)
		_, out = r.call("GET", "/v1/lobbies/"+id+"/signal", asHost, nil)
		if s := out["signals"].([]any); len(s) != 1 || s[0].(map[string]any)["data"] != "leave" {
			t.Fatalf("leave notice: %v", out)
		}
	})
}

func TestRelayCredentials(t *testing.T) {
	r := newRig(t, store.NewMemory(), Config{TurnSecret: "shared with coturn", TurnURLs: []string{"turn:relay.example:3478?transport=udp"}})
	token, id := r.session("Ann", 8)
	status, out := r.call("POST", "/v1/relay-credentials", auth(token), nil)
	if status != 200 || out["ttl"] != float64(3600) {
		t.Fatalf("credentials: %d %v", status, out)
	}
	// What coturn will check: the password is HMAC-SHA1 of the user name, which ends an hour from now.
	username := out["username"].(string)
	mac := hmac.New(sha1.New, []byte("shared with coturn"))
	mac.Write([]byte(username))
	if out["password"] != base64.StdEncoding.EncodeToString(mac.Sum(nil)) || !strings.HasSuffix(username, ":"+id) {
		t.Fatalf("credentials do not follow coturn's scheme: %v", out)
	}
	if status, _ := r.call("POST", "/v1/relay-credentials", nil, nil); status != http.StatusUnauthorized {
		t.Fatalf("credentials without a session: %d", status)
	}
	// Joining a lobby brings them along.
	lobby, _, _ := r.lobby(token, "Game", true)
	_, out = r.call("POST", "/v1/lobbies/"+lobby+"/join", auth(token), nil)
	if relay, ok := out["relay"].(map[string]any); !ok || relay["username"] == "" {
		t.Fatalf("join without relay credentials: %v", out)
	}
	// Without a secret there is no relay.
	none := newRig(t, store.NewMemory(), Config{})
	token, _ = none.session("Ann", 8)
	if status, _ := none.call("POST", "/v1/relay-credentials", auth(token), nil); status != 404 {
		t.Fatalf("credentials without a relay: %d", status)
	}
}

func TestLimits(t *testing.T) {
	r := newRig(t, store.NewMemory(), Config{WritePerMin: 20, CodePerMin: 5})
	// Guessing codes: after five tries in a minute the address waits.
	blocked := 0
	for i := 0; i < 12; i++ {
		if status, _ := r.call("GET", "/v1/lobbies/by-code/123456", nil, nil); status == http.StatusTooManyRequests {
			blocked++
		}
	}
	if blocked != 7 {
		t.Fatalf("code look-ups blocked: %d of 12", blocked)
	}
	r.clock.add(time.Minute)
	if status, _ := r.call("GET", "/v1/lobbies/by-code/123456", nil, nil); status != 404 {
		t.Fatalf("after a minute: %d", status)
	}
	// Writing: a burst is let through, then the address is slowed down; reading the list is not counted.
	limited := 0
	for i := 0; i < 40; i++ {
		if status, _ := r.call("POST", "/v1/session", nil, map[string]any{"name": "x", "version": 8}); status == http.StatusTooManyRequests {
			limited++
		}
	}
	if limited < 30 || limited > 35 {
		t.Fatalf("writes limited: %d of 40", limited)
	}
	if status, _ := r.call("GET", "/v1/lobbies", nil, nil); status != 200 {
		t.Fatalf("list while limited: %d", status)
	}
}

// Many lobbies: every code is six digits and no two are alike.
func TestCodesAreUnique(t *testing.T) {
	r := newRig(t, store.NewMemory(), Config{})
	seen := map[string]bool{}
	for i := 0; i < 300; i++ {
		token, _ := r.session("Host", 8)
		_, code, _ := r.lobby(token, "Game", i%2 == 0)
		if len(code) != 6 || seen[code] {
			t.Fatalf("code %q after %d lobbies", code, i)
		}
		seen[code] = true
	}
}

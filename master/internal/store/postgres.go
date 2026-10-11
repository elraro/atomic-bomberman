package store

import (
	"context"
	"errors"
	"strings"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgxpool"
)

// Schema is created on start if it is not there. Everything in it is short-lived.
const Schema = `
CREATE TABLE IF NOT EXISTS sessions (
	token_hash text PRIMARY KEY,
	id         text NOT NULL UNIQUE,
	name       text NOT NULL,
	version    integer NOT NULL,
	created    timestamptz NOT NULL,
	seen       timestamptz NOT NULL
);
CREATE TABLE IF NOT EXISTS lobbies (
	id              text PRIMARY KEY,
	code            text NOT NULL UNIQUE,
	host_token_hash text NOT NULL,
	host_session    text NOT NULL,
	name            text NOT NULL,
	public          boolean NOT NULL,
	version         integer NOT NULL,
	players         integer NOT NULL,
	seats           integer NOT NULL,
	phase           text NOT NULL,
	level           text NOT NULL,
	region          text NOT NULL,
	identity_key    text NOT NULL,
	certificate     text NOT NULL,
	candidates      text NOT NULL,
	created         timestamptz NOT NULL,
	seen            timestamptz NOT NULL
);
CREATE INDEX IF NOT EXISTS lobbies_public ON lobbies (created DESC) WHERE public;
CREATE TABLE IF NOT EXISTS signals (
	n        bigserial PRIMARY KEY,
	lobby_id text NOT NULL REFERENCES lobbies (id) ON DELETE CASCADE,
	receiver text NOT NULL,
	sender   text NOT NULL,
	data     text NOT NULL
);
CREATE INDEX IF NOT EXISTS signals_receiver ON signals (lobby_id, receiver, n);
`

type postgres struct {
	pool *pgxpool.Pool
}

// NewPostgres connects (url as for libpq) and makes sure the tables exist.
func NewPostgres(ctx context.Context, url string) (Store, error) {
	pool, err := pgxpool.New(ctx, url)
	if err != nil {
		return nil, err
	}
	if _, err := pool.Exec(ctx, Schema); err != nil {
		pool.Close()
		return nil, err
	}
	return &postgres{pool: pool}, nil
}

func (p *postgres) Close() { p.pool.Close() }

func (p *postgres) CreateSession(ctx context.Context, s Session) error {
	_, err := p.pool.Exec(ctx, `INSERT INTO sessions (token_hash, id, name, version, created, seen) VALUES ($1, $2, $3, $4, $5, $6)`,
		s.TokenHash, s.ID, s.Name, s.Version, s.Created, s.Seen)
	return err
}

func (p *postgres) SessionByTokenHash(ctx context.Context, hash string, now time.Time) (Session, error) {
	var s Session
	err := p.pool.QueryRow(ctx, `UPDATE sessions SET seen = $2 WHERE token_hash = $1 RETURNING token_hash, id, name, version, created, seen`, hash, now).
		Scan(&s.TokenHash, &s.ID, &s.Name, &s.Version, &s.Created, &s.Seen)
	if errors.Is(err, pgx.ErrNoRows) {
		return Session{}, ErrNotFound
	}
	return s, err
}

const lobbyColumns = `id, code, host_token_hash, host_session, name, public, version, players, seats, phase, level, region, identity_key, certificate, candidates, created, seen`

func scanLobby(row pgx.Row) (Lobby, error) {
	var l Lobby
	var candidates string
	err := row.Scan(&l.ID, &l.Code, &l.HostTokenHash, &l.HostSession, &l.Name, &l.Public, &l.Version, &l.Players, &l.Seats, &l.Phase, &l.Level, &l.Region,
		&l.IdentityKey, &l.Certificate, &candidates, &l.Created, &l.Seen)
	if errors.Is(err, pgx.ErrNoRows) {
		return Lobby{}, ErrNotFound
	}
	if candidates != "" {
		l.Candidates = strings.Split(candidates, "\n")
	}
	return l, err
}

func (p *postgres) CreateLobby(ctx context.Context, l Lobby) error {
	_, err := p.pool.Exec(ctx, `INSERT INTO lobbies (`+lobbyColumns+`) VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,$17)`,
		l.ID, l.Code, l.HostTokenHash, l.HostSession, l.Name, l.Public, l.Version, l.Players, l.Seats, l.Phase, l.Level, l.Region, l.IdentityKey, l.Certificate,
		strings.Join(l.Candidates, "\n"), l.Created, l.Seen)
	var pgErr *pgconn.PgError
	if errors.As(err, &pgErr) && pgErr.Code == "23505" && strings.Contains(pgErr.ConstraintName, "code") {
		return ErrCodeTaken
	}
	return err
}

func (p *postgres) Lobby(ctx context.Context, id string) (Lobby, error) {
	return scanLobby(p.pool.QueryRow(ctx, `SELECT `+lobbyColumns+` FROM lobbies WHERE id = $1`, id))
}

func (p *postgres) LobbyByCode(ctx context.Context, code string) (Lobby, error) {
	return scanLobby(p.pool.QueryRow(ctx, `SELECT `+lobbyColumns+` FROM lobbies WHERE code = $1`, code))
}

func (p *postgres) ListPublic(ctx context.Context, offset, limit int) ([]Lobby, int, error) {
	var total int
	if err := p.pool.QueryRow(ctx, `SELECT count(*) FROM lobbies WHERE public`).Scan(&total); err != nil {
		return nil, 0, err
	}
	rows, err := p.pool.Query(ctx, `SELECT `+lobbyColumns+` FROM lobbies WHERE public ORDER BY created DESC, id LIMIT $1 OFFSET $2`, limit, offset)
	if err != nil {
		return nil, 0, err
	}
	defer rows.Close()
	var out []Lobby
	for rows.Next() {
		l, err := scanLobby(rows)
		if err != nil {
			return nil, 0, err
		}
		out = append(out, l)
	}
	return out, total, rows.Err()
}

func (p *postgres) UpdateLobby(ctx context.Context, l Lobby) error {
	tag, err := p.pool.Exec(ctx, `UPDATE lobbies SET name=$2, public=$3, version=$4, players=$5, seats=$6, phase=$7, level=$8, region=$9, identity_key=$10,
		certificate=$11, candidates=$12, seen=$13 WHERE id=$1`,
		l.ID, l.Name, l.Public, l.Version, l.Players, l.Seats, l.Phase, l.Level, l.Region, l.IdentityKey, l.Certificate, strings.Join(l.Candidates, "\n"), l.Seen)
	if err == nil && tag.RowsAffected() == 0 {
		return ErrNotFound
	}
	return err
}

func (p *postgres) DeleteLobby(ctx context.Context, id string) error {
	tag, err := p.pool.Exec(ctx, `DELETE FROM lobbies WHERE id = $1`, id)
	if err == nil && tag.RowsAffected() == 0 {
		return ErrNotFound
	}
	return err
}

func (p *postgres) PushSignal(ctx context.Context, lobbyID, to string, s Signal) error {
	_, err := p.pool.Exec(ctx, `INSERT INTO signals (lobby_id, receiver, sender, data) VALUES ($1, $2, $3, $4)`, lobbyID, to, s.From, s.Data)
	var pgErr *pgconn.PgError
	if errors.As(err, &pgErr) && pgErr.Code == "23503" {
		return ErrNotFound
	}
	if err != nil {
		return err
	}
	// No more than MaxSignalsWaiting for one receiver: the oldest go.
	_, err = p.pool.Exec(ctx, `DELETE FROM signals WHERE lobby_id = $1 AND receiver = $2 AND n NOT IN
		(SELECT n FROM signals WHERE lobby_id = $1 AND receiver = $2 ORDER BY n DESC LIMIT $3)`, lobbyID, to, MaxSignalsWaiting)
	return err
}

func (p *postgres) PopSignals(ctx context.Context, lobbyID, to string) ([]Signal, error) {
	var exists bool
	if err := p.pool.QueryRow(ctx, `SELECT EXISTS (SELECT 1 FROM lobbies WHERE id = $1)`, lobbyID).Scan(&exists); err != nil {
		return nil, err
	}
	if !exists {
		return nil, ErrNotFound
	}
	rows, err := p.pool.Query(ctx, `WITH gone AS (DELETE FROM signals WHERE lobby_id = $1 AND receiver = $2 RETURNING n, sender, data)
		SELECT sender, data FROM gone ORDER BY n`, lobbyID, to)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []Signal
	for rows.Next() {
		var s Signal
		if err := rows.Scan(&s.From, &s.Data); err != nil {
			return nil, err
		}
		out = append(out, s)
	}
	return out, rows.Err()
}

func (p *postgres) Expire(ctx context.Context, lobbyBefore, sessionBefore time.Time) error {
	if _, err := p.pool.Exec(ctx, `DELETE FROM lobbies WHERE seen < $1`, lobbyBefore); err != nil {
		return err
	}
	_, err := p.pool.Exec(ctx, `DELETE FROM sessions WHERE seen < $1`, sessionBefore)
	return err
}

// abconfig makes and checks the signed documents of Atomic Bomberman's online
// play: keys, the configuration document, and the pin of the master's TLS key.
// See docs/operations/online.md.
package main

import (
	"crypto/ecdsa"
	"crypto/ed25519"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/sha256"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/hex"
	"encoding/json"
	"encoding/pem"
	"flag"
	"fmt"
	"math/big"
	"net"
	"os"
	"strings"
	"time"

	"github.com/elraro/atomic-bomberman/master/internal/signed"
	"github.com/elraro/atomic-bomberman/master/internal/tournament"
)

const usage = `abconfig keygen -out NAME                    make a key pair: NAME.key (keep it offline) and NAME.pub
abconfig sign -key NAME.key -in DOC.json -out SIGNED.json [-stamp] [-days 7] [-serial N]
                                             sign a document; -stamp sets serial, issued and expires in it
abconfig verify -pub HEX -in SIGNED.json     check a signed document and print it
abconfig pin -cert CERT.pem                  the pin of a TLS certificate's key, for "master.tls_pins"
abconfig example                             an example configuration document
abconfig cert -key ROOT.key -identity HEX -signing HEX -operator NAME [-contact TEXT] [-days 90] [-serial N] -out FILE
                                             a certificate for a game server: who answers for it, and its two keys
                                             (the server prints them: atomic_server --show-keys)
abconfig tournament-new -name NAME -organiser NAME -players FILE -servers FILE [-wins N] [-level N] [-scheme NAME]
                        [-play-time N] [-game-version V] [-protocol N] -out FILE.json
                                             a tournament file (not yet signed) with a knock-out bracket; FILE of
                                             players: "key name" a line, of servers: "identity-key signing-key address name"
abconfig tournament-result -in FILE.json -match ID -winner NAME [-records A.abrec,B.abrec] [-urls U1,U2]
                                             write a result into it (the records' hashes; the winner into the next round)
abconfig tournament-sign -key ORGANISER.key -in FILE.json -out SIGNED.json [-serial N]
                                             check it and sign it (each issue with a higher serial)
abconfig selfsigned -host NAME -out NAME     a TLS certificate signed by itself (NAME.crt, NAME.tls.key), for a master
                                             server without a certificate from an authority: the game goes by the pin
`

func die(format string, args ...any) {
	fmt.Fprintf(os.Stderr, format+"\n", args...)
	os.Exit(1)
}

func readKey(path string) ed25519.PrivateKey {
	raw, err := os.ReadFile(path)
	if err != nil {
		die("cannot read %s: %v", path, err)
	}
	seed, err := hex.DecodeString(strings.TrimSpace(string(raw)))
	if err != nil || len(seed) != ed25519.SeedSize {
		die("%s is not a key made by 'abconfig keygen'", path)
	}
	return ed25519.NewKeyFromSeed(seed)
}

func main() {
	if len(os.Args) < 2 {
		fmt.Fprint(os.Stderr, usage)
		os.Exit(2)
	}
	fs := flag.NewFlagSet(os.Args[1], flag.ExitOnError)
	out := fs.String("out", "", "output file")
	in := fs.String("in", "", "input file")
	keyFile := fs.String("key", "", "private key file")
	pub := fs.String("pub", "", "public key (hex)")
	cert := fs.String("cert", "", "certificate file (PEM)")
	stamp := fs.Bool("stamp", false, "set serial, issued and expires")
	days := fs.Int("days", 7, "with -stamp: days until the document expires (7 at most)")
	serial := fs.Int64("serial", 0, "with -stamp: the serial (default: the present time)")
	host := fs.String("host", "", "with selfsigned: the name or address the master server is reached by")
	identity := fs.String("identity", "", "with cert: the server's identity key (hex)")
	signing := fs.String("signing", "", "with cert: the server's signing key (hex)")
	operator := fs.String("operator", "", "with cert: who runs the server")
	contact := fs.String("contact", "", "with cert: how to reach them")
	name := fs.String("name", "", "tournament: its name")
	organiser := fs.String("organiser", "", "tournament: who runs it")
	playersFile := fs.String("players", "", "tournament-new: file of players, \"key name\" a line")
	serversFile := fs.String("servers", "", "tournament-new: file of servers, \"identity-key signing-key address name\" a line")
	wins := fs.Int("wins", 2, "tournament-new: round wins that decide a match")
	level := fs.Int("level", 0, "tournament-new: level 0-10, -1 for a random one")
	scheme := fs.String("scheme", "basic", "tournament-new: scheme")
	playTime := fs.Int("play-time", 150, "tournament-new: seconds a round")
	gameVersion := fs.String("game-version", "", "tournament-new: the release the matches are played with")
	protocol := fs.Int("protocol", 0, "tournament-new: its protocol version")
	matchID := fs.String("match", "", "tournament-result: the match")
	winner := fs.String("winner", "", "tournament-result: who won it")
	records := fs.String("records", "", "tournament-result: the match's record files, separated by commas")
	urls := fs.String("urls", "", "tournament-result: where they can be fetched, in the same order")
	_ = fs.Parse(os.Args[2:])

	switch os.Args[1] {
	case "keygen":
		if *out == "" {
			die("keygen needs -out NAME")
		}
		public, private, err := ed25519.GenerateKey(rand.Reader)
		if err != nil {
			die("no randomness: %v", err)
		}
		if err := os.WriteFile(*out+".key", []byte(hex.EncodeToString(private.Seed())+"\n"), 0o600); err != nil {
			die("%v", err)
		}
		if err := os.WriteFile(*out+".pub", []byte(hex.EncodeToString(public)+"\n"), 0o644); err != nil {
			die("%v", err)
		}
		fmt.Printf("public key: %s\nprivate key written to %s.key - keep it off any machine that faces the internet\n", hex.EncodeToString(public), *out)
	case "sign":
		if *keyFile == "" || *in == "" || *out == "" {
			die("sign needs -key, -in and -out")
		}
		document, err := os.ReadFile(*in)
		if err != nil {
			die("%v", err)
		}
		var fields map[string]any
		if err := json.Unmarshal(document, &fields); err != nil {
			die("%s is not a JSON object: %v", *in, err)
		}
		if *stamp {
			if *days < 1 || *days > 7 {
				die("a document may be issued for seven days at most")
			}
			now := time.Now().UTC().Truncate(time.Second)
			if *serial == 0 {
				*serial = now.Unix()
			}
			fields["serial"] = *serial
			fields["issued"] = now.Unix()
			fields["expires"] = now.Add(time.Duration(*days) * 24 * time.Hour).Unix()
			if document, err = json.MarshalIndent(fields, "", "  "); err != nil {
				die("%v", err)
			}
		}
		envelope, err := signed.Sign(readKey(*keyFile), document)
		if err != nil {
			die("%v", err)
		}
		if err := os.WriteFile(*out, append(envelope, '\n'), 0o644); err != nil {
			die("%v", err)
		}
		fmt.Printf("signed: %s\n", *out)
	case "verify":
		if *pub == "" || *in == "" {
			die("verify needs -pub and -in")
		}
		key, err := hex.DecodeString(*pub)
		if err != nil || len(key) != ed25519.PublicKeySize {
			die("-pub is not a public key")
		}
		envelope, err := os.ReadFile(*in)
		if err != nil {
			die("%v", err)
		}
		document, _, err := signed.Verify(envelope, []ed25519.PublicKey{key})
		if err != nil {
			die("NOT valid: %v", err)
		}
		fmt.Printf("%s\n", document)
	case "pin":
		if *cert == "" {
			die("pin needs -cert")
		}
		raw, err := os.ReadFile(*cert)
		if err != nil {
			die("%v", err)
		}
		block, _ := pem.Decode(raw)
		if block == nil {
			die("%s holds no PEM certificate", *cert)
		}
		c, err := x509.ParseCertificate(block.Bytes)
		if err != nil {
			die("%v", err)
		}
		// The key, not the certificate: it stays the same when the certificate is renewed with the same key.
		sum := sha256.Sum256(c.RawSubjectPublicKeyInfo)
		fmt.Println(hex.EncodeToString(sum[:]))
	case "example":
		fmt.Print(exampleConfig)
	case "cert":
		if *keyFile == "" || *out == "" || *operator == "" {
			die("cert needs -key, -identity, -signing, -operator and -out")
		}
		for name, value := range map[string]string{"-identity": *identity, "-signing": *signing} {
			if raw, err := hex.DecodeString(value); err != nil || len(raw) != 32 {
				die("%s must be a key of 64 hexadecimal digits", name)
			}
		}
		lifetime := *days
		if !flagGiven(fs, "days") {
			lifetime = 90
		}
		if lifetime < 1 || lifetime > 366 {
			die("a certificate is issued for 366 days at most")
		}
		now := time.Now().UTC().Truncate(time.Second)
		if *serial == 0 {
			*serial = now.Unix()
		}
		document, err := json.MarshalIndent(map[string]any{
			"kind":         "server-certificate",
			"serial":       *serial,
			"issued":       now.Unix(),
			"expires":      now.Add(time.Duration(lifetime) * 24 * time.Hour).Unix(),
			"operator":     map[string]string{"name": *operator, "contact": *contact},
			"identity_key": strings.ToLower(*identity),
			"signing_key":  strings.ToLower(*signing),
		}, "", "  ")
		if err != nil {
			die("%v", err)
		}
		envelope, err := signed.Sign(readKey(*keyFile), document)
		if err != nil {
			die("%v", err)
		}
		if err := os.WriteFile(*out, append(envelope, '\n'), 0o644); err != nil {
			die("%v", err)
		}
		fmt.Printf("certificate %d for %q, good for %d days: %s\n", *serial, *operator, lifetime, *out)
	case "tournament-new":
		if *name == "" || *playersFile == "" || *serversFile == "" || *out == "" {
			die("tournament-new needs -name, -players, -servers and -out")
		}
		f := tournament.File{Kind: "tournament", Name: *name, Organiser: *organiser, Players: []tournament.Player{}, Servers: []tournament.Server{},
			Rules: tournament.Rules{GameVersion: *gameVersion, Protocol: *protocol, Wins: *wins, Level: *level, Scheme: *scheme, PlayTime: *playTime, Enclosement: 1}}
		for _, line := range readLines(*playersFile) {
			key, playerName, ok := strings.Cut(line, " ")
			if !ok {
				die("%s: a line needs a key and a name: %q", *playersFile, line)
			}
			f.Players = append(f.Players, tournament.Player{Name: strings.TrimSpace(playerName), Key: strings.ToLower(key)})
		}
		for _, line := range readLines(*serversFile) {
			parts := strings.SplitN(line, " ", 4)
			if len(parts) != 4 {
				die("%s: a line needs an identity key, a signing key, an address and a name: %q", *serversFile, line)
			}
			f.Servers = append(f.Servers, tournament.Server{IdentityKey: strings.ToLower(parts[0]), SigningKey: strings.ToLower(parts[1]), Address: parts[2], Name: strings.TrimSpace(parts[3])})
		}
		f.Bracket()
		if err := f.Validate(); err != nil {
			die("%v", err)
		}
		writeTournament(*out, &f)
		fmt.Printf("%d players, %d matches: %s (set times and servers in it, then tournament-sign)\n", len(f.Players), len(f.Matches), *out)
	case "tournament-result":
		if *in == "" || *matchID == "" || *winner == "" {
			die("tournament-result needs -in, -match and -winner")
		}
		f := readTournament(*in)
		var contents [][]byte
		for _, file := range splitList(*records) {
			raw, err := os.ReadFile(file)
			if err != nil {
				die("%v", err)
			}
			contents = append(contents, raw)
		}
		if err := f.SetResult(*matchID, *winner, contents, splitList(*urls)); err != nil {
			die("%v", err)
		}
		if err := f.Validate(); err != nil {
			die("%v", err)
		}
		writeTournament(*in, f)
		fmt.Printf("match %s: won by %s, %d record(s)\n", *matchID, *winner, len(contents))
	case "tournament-sign":
		if *keyFile == "" || *in == "" || *out == "" {
			die("tournament-sign needs -key, -in and -out")
		}
		f := readTournament(*in)
		if err := f.Validate(); err != nil {
			die("not signed: %v", err)
		}
		now := time.Now().UTC().Truncate(time.Second)
		f.Kind = "tournament"
		f.Issued = now.Unix()
		f.Serial = now.Unix()
		if *serial != 0 {
			f.Serial = *serial
		}
		document, err := json.MarshalIndent(f, "", "  ")
		if err != nil {
			die("%v", err)
		}
		envelope, err := signed.Sign(readKey(*keyFile), document)
		if err != nil {
			die("%v", err)
		}
		if err := os.WriteFile(*out, append(envelope, '\n'), 0o644); err != nil {
			die("%v", err)
		}
		fmt.Printf("tournament %q, issue %d: %s\n", f.Name, f.Serial, *out)
	case "selfsigned":
		if *host == "" || *out == "" {
			die("selfsigned needs -host and -out")
		}
		certPEM, keyPEM, err := selfSigned(*host)
		if err != nil {
			die("%v", err)
		}
		if err := os.WriteFile(*out+".crt", certPEM, 0o644); err != nil {
			die("%v", err)
		}
		if err := os.WriteFile(*out+".tls.key", keyPEM, 0o600); err != nil {
			die("%v", err)
		}
		fmt.Printf("certificate: %s.crt, key: %s.tls.key (valid for ten years)\n", *out, *out)
	default:
		fmt.Fprint(os.Stderr, usage)
		os.Exit(2)
	}
}

func readLines(path string) []string {
	raw, err := os.ReadFile(path)
	if err != nil {
		die("%v", err)
	}
	var lines []string
	for _, line := range strings.Split(string(raw), "\n") {
		if line = strings.TrimSpace(line); line != "" && !strings.HasPrefix(line, "#") {
			lines = append(lines, line)
		}
	}
	return lines
}

func splitList(s string) []string {
	if s == "" {
		return nil
	}
	return strings.Split(s, ",")
}

func readTournament(path string) *tournament.File {
	raw, err := os.ReadFile(path)
	if err != nil {
		die("%v", err)
	}
	var f tournament.File
	if err := json.Unmarshal(raw, &f); err != nil {
		die("%s is not a tournament file: %v", path, err)
	}
	return &f
}

func writeTournament(path string, f *tournament.File) {
	document, err := json.MarshalIndent(f, "", "  ")
	if err != nil {
		die("%v", err)
	}
	if err := os.WriteFile(path, append(document, '\n'), 0o644); err != nil {
		die("%v", err)
	}
}

func flagGiven(fs *flag.FlagSet, name string) bool {
	given := false
	fs.Visit(func(f *flag.Flag) { given = given || f.Name == name })
	return given
}

// selfSigned makes an ECDSA P-256 key and a certificate for it, valid for ten years.
func selfSigned(host string) (certPEM, keyPEM []byte, err error) {
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return nil, nil, err
	}
	number, err := rand.Int(rand.Reader, new(big.Int).Lsh(big.NewInt(1), 120))
	if err != nil {
		return nil, nil, err
	}
	template := x509.Certificate{
		SerialNumber:          number,
		Subject:               pkix.Name{CommonName: host},
		NotBefore:             time.Now().Add(-time.Hour),
		NotAfter:              time.Now().AddDate(10, 0, 0),
		KeyUsage:              x509.KeyUsageDigitalSignature,
		ExtKeyUsage:           []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		BasicConstraintsValid: true,
	}
	if ip := net.ParseIP(host); ip != nil {
		template.IPAddresses = []net.IP{ip}
	} else {
		template.DNSNames = []string{host}
	}
	der, err := x509.CreateCertificate(rand.Reader, &template, &template, &key.PublicKey, key)
	if err != nil {
		return nil, nil, err
	}
	keyDER, err := x509.MarshalPKCS8PrivateKey(key)
	if err != nil {
		return nil, nil, err
	}
	return pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der}), pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: keyDER}), nil
}

const exampleConfig = `{
  "kind": "configuration",
  "master": {
    "url": "https://master.example.org:8443",
    "tls_pins": ["<output of: abconfig pin -cert fullchain.pem>"]
  },
  "stun": ["turn.example.org:3478"],
  "turn": [
    {"host": "turn.example.org", "port": 3478, "transport": "udp"},
    {"host": "turn.example.org", "port": 3478, "transport": "tcp"},
    {"host": "turn.example.org", "port": 443, "transport": "tls"}
  ],
  "protocol": {"min": 8, "current": 8},
  "status": {"state": "ok", "message": ""},
  "trust": {"roots": [], "revoked": []},
  "flags": {}
}
`

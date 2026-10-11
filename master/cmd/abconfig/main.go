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
)

const usage = `abconfig keygen -out NAME                    make a key pair: NAME.key (keep it offline) and NAME.pub
abconfig sign -key NAME.key -in DOC.json -out SIGNED.json [-stamp] [-days 7] [-serial N]
                                             sign a document; -stamp sets serial, issued and expires in it
abconfig verify -pub HEX -in SIGNED.json     check a signed document and print it
abconfig pin -cert CERT.pem                  the pin of a TLS certificate's key, for "master.tls_pins"
abconfig example                             an example configuration document
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

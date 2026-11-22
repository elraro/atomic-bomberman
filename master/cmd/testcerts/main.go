// testcerts makes a small certificate authority and the server certificates the integration
// test (tests/integration/online.sh) needs to check how the game judges a TLS server by the
// roots its configuration names: a chain of the kind Let's Encrypt hands out (root, then an
// intermediate, then the server's certificate, the server sending the last two), and the
// ones that must be refused. For tests only: it is not part of the published image.
//
//	testcerts -out DIR -names localhost,127.0.0.1,127.0.0.2
//
// In DIR: root.crt (the authority to name in the configuration), other-root.crt (an
// authority nobody names), and, each as NAME.crt (the server's certificate followed by
// the intermediate) and NAME.key:
//
//	good       made out to the names, from root by way of the intermediate
//	wrongname  made out to wrong.invalid only
//	expired    made out to the names, but expired yesterday
//	stranger   made out to the names, from other-root
package main

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/pem"
	"flag"
	"fmt"
	"math/big"
	"net"
	"os"
	"path/filepath"
	"strings"
	"time"
)

type issued struct {
	cert *x509.Certificate
	der  []byte
	key  *ecdsa.PrivateKey
}

var serial int64 = 1000

func issue(template *x509.Certificate, parent *issued) issued {
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		die("key: %v", err)
	}
	serial++
	template.SerialNumber = big.NewInt(serial)
	signer, parentCert := key, template
	if parent != nil {
		signer, parentCert = parent.key, parent.cert
	}
	der, err := x509.CreateCertificate(rand.Reader, template, parentCert, &key.PublicKey, signer)
	if err != nil {
		die("certificate: %v", err)
	}
	cert, err := x509.ParseCertificate(der)
	if err != nil {
		die("certificate: %v", err)
	}
	return issued{cert, der, key}
}

func authority(name string, parent *issued) issued {
	now := time.Now()
	return issue(&x509.Certificate{
		Subject:               pkix.Name{CommonName: name},
		NotBefore:             now.Add(-24 * time.Hour),
		NotAfter:              now.Add(10 * 365 * 24 * time.Hour),
		IsCA:                  true,
		BasicConstraintsValid: true,
		KeyUsage:              x509.KeyUsageCertSign | x509.KeyUsageCRLSign,
	}, parent)
}

func server(names []string, notBefore, notAfter time.Time, parent *issued) issued {
	t := &x509.Certificate{
		Subject:     pkix.Name{CommonName: names[0]},
		NotBefore:   notBefore,
		NotAfter:    notAfter,
		KeyUsage:    x509.KeyUsageDigitalSignature,
		ExtKeyUsage: []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
	}
	for _, n := range names {
		if ip := net.ParseIP(n); ip != nil {
			t.IPAddresses = append(t.IPAddresses, ip)
		} else {
			t.DNSNames = append(t.DNSNames, n)
		}
	}
	return issue(t, parent)
}

func die(format string, args ...any) {
	fmt.Fprintf(os.Stderr, "testcerts: "+format+"\n", args...)
	os.Exit(1)
}

func write(path string, blocks ...*pem.Block) {
	var out []byte
	for _, b := range blocks {
		out = append(out, pem.EncodeToMemory(b)...)
	}
	if err := os.WriteFile(path, out, 0o600); err != nil {
		die("%v", err)
	}
}

func certBlock(i issued) *pem.Block { return &pem.Block{Type: "CERTIFICATE", Bytes: i.der} }

func main() {
	out := flag.String("out", "", "directory to write into")
	namesFlag := flag.String("names", "localhost,127.0.0.1", "names and addresses the servers' certificates are made out to")
	flag.Parse()
	if *out == "" {
		die("-out is needed")
	}
	names := strings.Split(*namesFlag, ",")
	if err := os.MkdirAll(*out, 0o700); err != nil {
		die("%v", err)
	}

	root := authority("Test Root", nil)
	intermediate := authority("Test Intermediate", &root)
	otherRoot := authority("Other Root", nil)
	otherIntermediate := authority("Other Intermediate", &otherRoot)
	write(filepath.Join(*out, "root.crt"), certBlock(root))
	write(filepath.Join(*out, "other-root.crt"), certBlock(otherRoot))

	now := time.Now()
	leaves := map[string]struct {
		leaf  issued
		chain issued
	}{
		"good":      {server(names, now.Add(-time.Hour), now.Add(90*24*time.Hour), &intermediate), intermediate},
		"wrongname": {server([]string{"wrong.invalid"}, now.Add(-time.Hour), now.Add(90*24*time.Hour), &intermediate), intermediate},
		"expired":   {server(names, now.Add(-90*24*time.Hour), now.Add(-24*time.Hour), &intermediate), intermediate},
		"stranger":  {server(names, now.Add(-time.Hour), now.Add(90*24*time.Hour), &otherIntermediate), otherIntermediate},
	}
	for name, l := range leaves {
		keyDER, err := x509.MarshalPKCS8PrivateKey(l.leaf.key)
		if err != nil {
			die("%v", err)
		}
		write(filepath.Join(*out, name+".crt"), certBlock(l.leaf), certBlock(l.chain))
		write(filepath.Join(*out, name+".key"), &pem.Block{Type: "PRIVATE KEY", Bytes: keyDER})
	}
}

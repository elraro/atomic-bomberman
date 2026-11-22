# Let's Encrypt's roots

`isrg-roots.pem` holds the two root certificates of Let's Encrypt (Internet Security
Research Group). With a proxy in front (`PROXY` in `../.env`), `setup.sh` puts them into the
configuration as `tls_roots` of the master and of the relay over TLS: a game then takes a
server whose certificate chain leads to one of them and is made out to the name it connected
to (`docs/specifications/online-play.md`, section 2).

| Root | Key | Certificate valid until | SHA-256 fingerprint |
|---|---|---|---|
| ISRG Root X1 | RSA 4096 | 2035-06-04 | `96:BC:EC:06:26:49:76:F3:74:60:77:9A:CF:28:C5:A7:CF:E8:A3:C0:AA:E1:1A:8F:FC:EE:05:C0:BD:DF:08:C6` |
| ISRG Root X2 | ECDSA P-384 | 2040-09-17 | `69:72:9B:8E:15:A8:6E:FC:17:7A:57:AF:B7:17:1D:FC:64:AD:D2:8C:2F:CA:8C:F1:50:7E:34:45:3C:CB:14:70` |

Fetched on 2026-11-22 from <https://letsencrypt.org/certs/isrgrootx1.pem> and
<https://letsencrypt.org/certs/isrg-root-x2.pem>; both identical to the copies in Ubuntu's
system trust store (`/etc/ssl/certs/ISRG_Root_X1.pem`, `ISRG_Root_X2.pem`). Public
certificates, in every browser's and system's trust store.

Let's Encrypt says (<https://letsencrypt.org/certificates/>, November 2026) that root
programmes trust X1 until about 2030-06-04 and X2 until about 2035-09-04, and that two new
roots, YE and YR, are submitted but not yet widely trusted. When certificates start to lead
to other roots, add them here (or edit `data/config.json`), check them against that page,
and sign the configuration again (`./sign.sh`). No new release of the game is needed.

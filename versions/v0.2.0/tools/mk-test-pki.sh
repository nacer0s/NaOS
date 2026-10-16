#!/bin/sh
# tools/mk-test-pki.sh — fresh localhost TEST PKI for get-page-tls testing.
# Makes: testca.key/testca.der (CA), srv.key/srv.der/srv.crt (server leaf
# with SAN IP:10.0.2.2,DNS:naos.test), and rewrites kernel/net/tls_ca.h
# with the new trust anchor. The old anchor's key is gone; rotation is
# required for any further TLS interop test. TEST ONLY - replace before
# any real use.
# Usage: ./tools/mk-test-pki.sh [outdir]   (default: /tmp/naos-pki)
set -e
OUT=${1:-/tmp/naos-pki}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT"
cd "$OUT"

openssl req -x509 -newkey rsa:2048 -keyout testca.key -out testca.der \
  -outform DER -days 365 -nodes -subj "/CN=NaOS Test CA" 2>/dev/null
openssl req -newkey rsa:2048 -keyout srv.key -out srv.csr -nodes \
  -subj "/CN=naos.test" 2>/dev/null
cat > srv.ext <<'EOF'
subjectAltName = IP:10.0.2.2,DNS:naos.test
basicConstraints = CA:FALSE
keyUsage = digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
EOF
openssl x509 -req -in srv.csr -CA testca.der -CAform DER -CAkey testca.key \
  -out srv.der -outform DER -days 365 -extfile srv.ext 2>/dev/null
openssl x509 -in srv.der -inform DER -out srv.crt 2>/dev/null
rm -f srv.csr

# Emit kernel/net/tls_ca.h in the repo's exact format (12 bytes/line).
python3 - "$OUT/testca.der" "$HERE/../kernel/net/tls_ca.h" <<'EOF'
import sys
der = open(sys.argv[1], 'rb').read()
out = []
out.append("/* NaOS TLS TEST trust anchor (public cert only, no key).")
out.append(" * Generated for localhost testing - REPLACE before any real use.")
out.append(" * The matching private key never enters this repo. */")
out.append("#ifndef NAOS_TLS_CA_H")
out.append("#define NAOS_TLS_CA_H")
out.append("#include <stdint.h>")
out.append("static const uint8_t TLS_CA_DER[%d] = {" % len(der))
for i in range(0, len(der), 12):
    out.append(" " + ",".join("0x%02x" % b for b in der[i:i+12]) + ",")
out.append("};")
out.append("static const uint32_t TLS_CA_DER_LEN = sizeof(TLS_CA_DER);")
out.append("#endif")
open(sys.argv[2], 'w').write("\n".join(out) + "\n")
print("tls_ca.h: %d bytes DER" % len(der))
EOF
echo "pki: $OUT (testca.der srv.der srv.key); anchor rotated"
openssl x509 -in srv.der -inform DER -noout -subject -ext subjectAltName

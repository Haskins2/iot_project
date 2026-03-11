#!/usr/bin/env bash
# ======================================================
# generate-certs.sh — run once before docker compose up
# Creates a private CA and broker certificate for TLS.
#
# Usage: ./generate-certs.sh [broker-hostname]
# Default hostname: localhost
#
# when run on cloud use oracle public ip
#
# Output (mosquitto/certs/):
#   ca.crt      — copy this to your devices (public, not sensitive)
#   broker.crt  — stays on the server
#   broker.key  — stays on the server, keep secret
# =========================================================
set -euo pipefail

CERTS_DIR="./mosquitto/certs"
BROKER_CN="${1:-localhost}"

echo "==> Generating certs in ${CERTS_DIR} (CN=${BROKER_CN})"
mkdir -p "${CERTS_DIR}"

# CA
openssl genrsa -out "${CERTS_DIR}/ca.key" 4096
openssl req -new -x509 -days 3650 \
  -key  "${CERTS_DIR}/ca.key" \
  -out  "${CERTS_DIR}/ca.crt" \
  -subj "/CN=IoT-CA/O=IoT/C=GB"

# Broker key + CSR
openssl genrsa -out "${CERTS_DIR}/broker.key" 2048
openssl req -new \
  -key  "${CERTS_DIR}/broker.key" \
  -out  "${CERTS_DIR}/broker.csr" \
  -subj "/CN=${BROKER_CN}/O=IoT/C=GB"

# Sign with SAN so modern TLS stacks accept it
cat > /tmp/broker_ext.cnf <<EOF
[SAN]
subjectAltName=DNS:${BROKER_CN},DNS:mosquitto,DNS:localhost,IP:127.0.0.1
EOF

openssl x509 -req -days 825 \
  -in     "${CERTS_DIR}/broker.csr" \
  -CA     "${CERTS_DIR}/ca.crt" \
  -CAkey  "${CERTS_DIR}/ca.key" \
  -CAcreateserial \
  -out    "${CERTS_DIR}/broker.crt" \
  -extfile /tmp/broker_ext.cnf \
  -extensions SAN

chmod 600 "${CERTS_DIR}/ca.key" "${CERTS_DIR}/broker.key"
chmod 644 "${CERTS_DIR}/ca.crt" "${CERTS_DIR}/broker.crt"
rm -f "${CERTS_DIR}/broker.csr" /tmp/broker_ext.cnf

echo ""
echo "==> Done:"
ls -lh "${CERTS_DIR}"
echo ""
echo "==> Distribute ca.crt to your devices — it is public and not sensitive."
echo "    Never commit ca.key or broker.key."
#!/usr/bin/env bash
# =============================================================================
# generate-certs.sh — run once before docker compose up
#
# Creates a private CA and signs certificates for the broker, devices,
# and backend services. The CA cert is the only file distributed to devices.
#
# Usage:
#   ./generate-certs.sh broker  <hostname>      broker cert (use VM IP or domain)
#   ./generate-certs.sh device  <device-id>     per-device cert  e.g. esp-a1b2c3
#   ./generate-certs.sh service <service-name>  service cert     e.g. subscriber-service
#
# The CN of each client cert becomes the MQTT username via use_identity_as_username.
# Device CN must match the device_id used in the topic: devices/{device_id}/#
#
# Prototype:   device.key stored plaintext on flash — physical access is a known risk
# Production:  enable eFuse flash encryption — key becomes physically unreadable
# =============================================================================
set -euo pipefail

CERTS_DIR="./mosquitto/certs"
mkdir -p "$CERTS_DIR"

MODE="${1:-}"
NAME="${2:-}"

# CA — generated once, reused for all certs
generate_ca() {
  if [ -f "$CERTS_DIR/ca.key" ] && [ -f "$CERTS_DIR/ca.crt" ]; then
    echo "==> CA already exists — reusing"
    return
  fi
  echo "==> Generating CA..."
  openssl genrsa -out "$CERTS_DIR/ca.key" 4096
  openssl req -new -x509 -days 3650 \
    -key  "$CERTS_DIR/ca.key" \
    -out  "$CERTS_DIR/ca.crt" \
    -subj "/CN=IoT-CA/O=IoT/C=GB"
  chmod 600 "$CERTS_DIR/ca.key"
  chmod 644 "$CERTS_DIR/ca.crt"
  echo "==> CA ready: $CERTS_DIR/ca.crt"
}


# Broker cert — needs SAN for hostname verification
generate_broker() {
  local cn="$1"
  echo "==> Generating broker cert (CN=${cn})..."
  openssl genrsa -out "$CERTS_DIR/broker.key" 2048
  openssl req -new \
    -key  "$CERTS_DIR/broker.key" \
    -out  "$CERTS_DIR/broker.csr" \
    -subj "/CN=${cn}/O=IoT/C=GB"

  cat > /tmp/broker_ext.cnf << EXTEOF
[SAN]
subjectAltName=DNS:${cn},DNS:mosquitto,DNS:localhost,IP:127.0.0.1
EXTEOF

  openssl x509 -req -days 825 \
    -in       "$CERTS_DIR/broker.csr" \
    -CA       "$CERTS_DIR/ca.crt" \
    -CAkey    "$CERTS_DIR/ca.key" \
    -CAcreateserial \
    -out      "$CERTS_DIR/broker.crt" \
    -extfile  /tmp/broker_ext.cnf \
    -extensions SAN

  chmod 600 "$CERTS_DIR/broker.key"
  chmod 644 "$CERTS_DIR/broker.crt"
  rm -f "$CERTS_DIR/broker.csr" /tmp/broker_ext.cnf
  echo "==> Broker cert ready"
}


# Client cert — for devices and backend services
# CN = MQTT username = ACL identity
generate_client() {
  local cn="$1"
  echo "==> Generating client cert (CN=${cn})..."
  openssl genrsa -out "$CERTS_DIR/${cn}.key" 2048
  openssl req -new \
    -key  "$CERTS_DIR/${cn}.key" \
    -out  "$CERTS_DIR/${cn}.csr" \
    -subj "/CN=${cn}/O=IoT/C=GB"
  openssl x509 -req -days 825 \
    -in     "$CERTS_DIR/${cn}.csr" \
    -CA     "$CERTS_DIR/ca.crt" \
    -CAkey  "$CERTS_DIR/ca.key" \
    -CAcreateserial \
    -out    "$CERTS_DIR/${cn}.crt"

  chmod 600 "$CERTS_DIR/${cn}.key"
  chmod 644 "$CERTS_DIR/${cn}.crt"
  rm -f "$CERTS_DIR/${cn}.csr"
  echo "==> Client cert ready: ${cn}.crt + ${cn}.key"
  echo "    Flash onto device along with ca.crt"
}


# Main
generate_ca

case "$MODE" in
  broker)
    [ -z "$NAME" ] && { echo "Usage: $0 broker <hostname>"; exit 1; }
    generate_broker "$NAME"
    ;;
  device|service)
    [ -z "$NAME" ] && { echo "Usage: $0 device <device-id>"; exit 1; }
    generate_client "$NAME"
    ;;
  *)
    echo "Usage:"
    echo "  $0 broker  <hostname>      e.g. $0 broker 129.1.2.3"
    echo "  $0 device  <device-id>     e.g. $0 device esp-a1b2c3"
    echo "  $0 service <service-name>  e.g. $0 service subscriber-service"
    exit 1
    ;;
esac

echo ""
echo "==> Files in ${CERTS_DIR}:"
ls -lh "$CERTS_DIR"
echo ""
echo "==> ca.crt is public — safe to distribute to all devices"
echo "    *.key files are private — never commit to git"
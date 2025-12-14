#!/bin/bash

# Generate TLS certificates for StreamTablet
# Run this script once to generate server and client certificates

set -e

CERT_DIR="$(dirname "$0")"
cd "$CERT_DIR"

echo "Generating Certificate Authority..."
openssl genrsa -out ca.key 4096
openssl req -new -x509 -days 3650 -key ca.key -out ca.crt \
    -subj "/CN=StreamTablet CA/O=StreamTablet/C=US"

echo "Generating Server Certificate..."
openssl genrsa -out server.key 2048
openssl req -new -key server.key -out server.csr \
    -subj "/CN=StreamTablet Server/O=StreamTablet/C=US"
openssl x509 -req -days 365 -in server.csr -CA ca.crt -CAkey ca.key \
    -CAcreateserial -out server.crt

echo "Generating Client Certificate..."
openssl genrsa -out client.key 2048
openssl req -new -key client.key -out client.csr \
    -subj "/CN=StreamTablet Client/O=StreamTablet/C=US"
openssl x509 -req -days 365 -in client.csr -CA ca.crt -CAkey ca.key \
    -CAcreateserial -out client.crt

# Create PKCS12 for Android (includes private key)
echo "Creating PKCS12 for Android..."
openssl pkcs12 -export -out client.p12 -inkey client.key -in client.crt \
    -certfile ca.crt -passout pass:streamtablet

# Cleanup CSR files
rm -f *.csr

echo ""
echo "Certificates generated successfully!"
echo ""
echo "Files created:"
echo "  ca.crt       - Certificate Authority (copy to Android)"
echo "  server.crt   - Server certificate"
echo "  server.key   - Server private key"
echo "  client.crt   - Client certificate"
echo "  client.key   - Client private key"
echo "  client.p12   - Client PKCS12 for Android (password: streamtablet)"
echo ""
echo "To use TLS:"
echo "  1. Keep ca.crt, server.crt, server.key on the server"
echo "  2. Copy ca.crt and client.p12 to your Android device"

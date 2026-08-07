#!/usr/bin/env bash
set -euo pipefail

out_dir="${1:-./lab/certs}"
mkdir -p "$out_dir"

make_cert() {
    local name="$1"
    local cn="$2"

    openssl req \
        -x509 \
        -newkey rsa:2048 \
        -sha256 \
        -nodes \
        -days 7 \
        -keyout "$out_dir/${name}.key.pem" \
        -out "$out_dir/${name}.cert.pem" \
        -subj "/CN=${cn}" \
        -addext "subjectAltName=DNS:${cn}"
}

make_cert backend-a backend.test
make_cert backend-b backend.test

chmod 0600 "$out_dir"/*.key.pem
chmod 0644 "$out_dir"/*.cert.pem

printf 'Generated test identities in %s\n' "$out_dir"
printf '\nCertificate A:\n'
openssl x509 -in "$out_dir/backend-a.cert.pem" -noout -subject -issuer -fingerprint -sha256
printf '\nCertificate B:\n'
openssl x509 -in "$out_dir/backend-b.cert.pem" -noout -subject -issuer -fingerprint -sha256

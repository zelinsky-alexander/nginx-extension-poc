#!/usr/bin/env python3

import http.server
import ssl
import sys


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        body = b"keepalive-test\n"

        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "keep-alive")
        self.end_headers()

        self.wfile.write(body)

    def log_message(self, fmt, *args):
        print("%s - %s" % (self.client_address, fmt % args))


port = int(sys.argv[1])
cert = sys.argv[2]
key = sys.argv[3]

server = http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler)

context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(certfile=cert, keyfile=key)

server.socket = context.wrap_socket(
    server.socket,
    server_side=True,
)

print(f"HTTPS keepalive server listening on 127.0.0.1:{port}")
server.serve_forever()


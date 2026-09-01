import http.server
import ssl
import sys
import threading


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def setup(self):
        super().setup()
        with self.server.observation_lock:
            self.server.connection_count += 1
            self.connection_id = self.server.connection_count

    def do_GET(self):
        bodies = {
            "/": b"ok",
            "/one": b"one",
            "/two": b"two",
            "/close": b"close",
            "/silent-close": b"silent",
        }
        body = bodies.get(self.path, b"not found")
        status = 200 if self.path in bodies else 404
        with self.server.observation_lock:
            if self.server.report_path:
                with open(self.server.report_path, "a", encoding="ascii") as report:
                    report.write(f"{self.connection_id} {self.path}\n")
                    report.flush()
        self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        if self.path == "/close":
            self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.wfile.flush()
        if self.path in ("/close", "/silent-close"):
            self.close_connection = True

    def log_message(self, fmt, *args):
        pass


port = int(sys.argv[1])
server = http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler)
server.connection_count = 0
server.observation_lock = threading.Lock()
server.report_path = sys.argv[5] if len(sys.argv) >= 6 else None
if server.report_path:
    with open(server.report_path, "w", encoding="ascii"):
        pass
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(sys.argv[2], sys.argv[3])
server.socket = context.wrap_socket(server.socket, server_side=True)
if len(sys.argv) >= 5:
    with open(sys.argv[4], "w", encoding="ascii") as ready:
        ready.write(f"{server.server_address[1]}\n")
server.serve_forever()

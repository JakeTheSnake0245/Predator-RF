import http.server
import socketserver
import os

PORT = 5000
HOST = "0.0.0.0"

class Handler(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/" or self.path == "/index.html":
            self._serve_file("index.html")
        elif self.path == "/preview" or self.path == "/preview.html":
            self._serve_file("preview.html")
        else:
            super().do_GET()

    def _serve_file(self, filename):
        try:
            with open(filename, "rb") as f:
                body = f.read()
        except FileNotFoundError:
            self.send_error(404, f"{filename} not found")
            return
        self.send_response(200)
        self.send_header("Content-type", "text/html; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *args):
        print(f"[{self.address_string()}] {format % args}")

class ReusableTCPServer(socketserver.TCPServer):
    # Allow rebinding the port immediately after the previous process exits,
    # so workflow restarts don't fail with "Address already in use" while
    # the kernel is still holding the socket in TIME_WAIT.
    allow_reuse_address = True


with ReusableTCPServer((HOST, PORT), Handler) as httpd:
    print(f"Serving Predator SDR project page on http://{HOST}:{PORT}")
    httpd.serve_forever()

import http.server
import mimetypes
import os
import socketserver

PORT = 5000
HOST = "0.0.0.0"

class Handler(http.server.SimpleHTTPRequestHandler):
    # Resolved absolute base directories.  All path arithmetic is done
    # against these so that a crafted URL like /dashboard/../../etc/passwd
    # is caught before open() is ever called.
    _DASHBOARD_BASE = os.path.realpath(os.path.join(os.path.dirname(__file__),
                                                     "dashboard"))
    _MAPS_BASE      = os.path.realpath(os.path.join(os.path.dirname(__file__),
                                                     "root", "res", "maps"))

    @staticmethod
    def _safe_path(base: str, rel: str) -> str | None:
        """Return realpath of base/rel only when it stays inside base.

        Rejects empty rel, absolute paths, ``..`` traversal, URL-encoded
        variants and any resolved path that escapes the base directory.
        Returns None on any violation.
        """
        # Strip leading slashes/dots so os.path.join can't treat rel as absolute
        rel = rel.lstrip("/")
        if not rel:
            return None
        candidate = os.path.realpath(os.path.join(base, rel))
        # Must start with base + sep to prevent /basefoo matching /base prefix
        if not (candidate == base or candidate.startswith(base + os.sep)):
            return None
        return candidate

    def do_GET(self):
        if self.path == "/" or self.path == "/index.html":
            self._serve_file("index.html", "text/html; charset=utf-8")
        elif self.path == "/preview" or self.path == "/preview.html":
            self._serve_file("preview.html", "text/html; charset=utf-8")
        elif self.path in ("/dashboard", "/dashboard/"):
            self._serve_file(os.path.join("dashboard", "index.html"),
                             "text/html; charset=utf-8")
        elif self.path.startswith("/dashboard/"):
            rel = self.path[len("/dashboard/"):]
            safe = self._safe_path(self._DASHBOARD_BASE, rel)
            if safe is None:
                self.send_error(403, "Forbidden")
                return
            mime, _ = mimetypes.guess_type(safe)
            self._serve_file(safe, mime or "application/octet-stream")
        elif self.path.startswith("/maps/"):
            rel = self.path[len("/maps/"):]
            safe = self._safe_path(self._MAPS_BASE, rel)
            if safe is None:
                self.send_error(403, "Forbidden")
                return
            mime, _ = mimetypes.guess_type(safe)
            self._serve_file(safe, mime or "application/octet-stream")
        else:
            super().do_GET()

    def _serve_file(self, filename, content_type="text/html; charset=utf-8"):
        try:
            with open(filename, "rb") as f:
                body = f.read()
        except FileNotFoundError:
            self.send_error(404, f"{filename} not found")
            return
        self.send_response(200)
        self.send_header("Content-type", content_type)
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
    print(f"CoC Dashboard preview: http://{HOST}:{PORT}/dashboard")
    httpd.serve_forever()

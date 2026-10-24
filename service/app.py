"""Small public service for the deployed engine (standard library only).

GET /          short page explaining the project, with links
GET /health    {"status": "ok", "version": ...}
GET /verify    runs a fresh end-to-end check: random orders (new seed each call) -> engine vs
               independent reference, byte for byte, plus speed. Cached for 60 s to keep it cheap.
"""
import json, os, random, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "ci"))
import verify_run

VERSION = os.environ.get("APP_VERSION", "dev")
N_ORDERS = int(os.environ.get("VERIFY_ORDERS", "50000"))
_lock, _cache = threading.Lock(), {"t": 0.0, "r": None}

PAGE = """<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Matching Engine</title><style>body{{font-family:system-ui;max-width:720px;margin:40px auto;padding:0 16px;line-height:1.5}}
code{{background:#f3f3f3;padding:2px 4px}}</style></head><body>
<h1>Price-time-priority matching engine</h1>
<p>A C++17 order book that maintains itself: every change is written, reviewed, tested byte for byte against an
independent reference exchange, speed-checked, security-scanned and deployed by GitLab Duo agents.</p>
<p>Version <code>{v}</code>. Try <a href="/verify">/verify</a> to run a fresh check right now, or <a href="/health">/health</a>.</p>
</body></html>"""


def verify():
    with _lock:
        if _cache["r"] is None or time.time() - _cache["t"] > 60:
            r = verify_run.run(N_ORDERS, seed=random.randint(1, 10**9))
            r["version"] = VERSION
            _cache.update(t=time.time(), r=r)
        return _cache["r"]


class H(BaseHTTPRequestHandler):
    def _send(self, code, body, ctype="application/json"):
        data = body.encode() if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path == "/health":
            return self._send(200, json.dumps({"status": "ok", "version": VERSION}))
        if self.path.startswith("/verify"):
            try:
                r = verify()
                return self._send(200 if r["identical"] else 500, json.dumps(r))
            except Exception as e:
                return self._send(500, json.dumps({"identical": False, "error": str(e)}))
        if self.path == "/":
            return self._send(200, PAGE.format(v=VERSION), "text/html; charset=utf-8")
        return self._send(404, json.dumps({"error": "not found"}))

    def log_message(self, *a):
        pass


if __name__ == "__main__":
    port = int(os.environ.get("PORT", "8080"))
    ThreadingHTTPServer(("0.0.0.0", port), H).serve_forever()

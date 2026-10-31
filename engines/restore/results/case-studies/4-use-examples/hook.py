"""A stand-in for a chat or paging webhook: listens on 127.0.0.1:PORT and
appends each POST body, with its Content-Type, to FILE.

  python3 -I hook.py PORT FILE
"""
import http.server, sys
port, path = int(sys.argv[1]), sys.argv[2]
class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        with open(path, "ab") as f:
            f.write(("POST %s  Content-Type: %s\n" % (self.path, self.headers.get("Content-Type"))).encode())
            f.write(body + b"\n")
        self.send_response(204); self.end_headers()
    def log_message(self, *a): pass
http.server.HTTPServer(("127.0.0.1", port), H).serve_forever()

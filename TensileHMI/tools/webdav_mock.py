#!/usr/bin/env python3
"""Tiny local WebDAV mock of the Nextcloud endpoint used by tensile_upload.

Stdlib only. Implements just what the HMI uses, with Nextcloud/SabreDAV
semantics as far as they are known:

  PROPFIND  Depth 0/1 -> 207 multistatus (d:/oc:/nc: namespaces, &quot;-quoted
            getetag, getcontentlength reported 404 for collections), 404 if missing
  MKCOL     201 created, 405 exists, 409 parent missing
  PUT       201 created / 204 replaced, ETag + OC-ETag headers,
            If-None-Match: * on existing -> 412, If-Match mismatch -> 412,
            409 parent missing
  any       401 + WWW-Authenticate on missing/bad Basic auth

Fault injection: --inject PUT:503:7 makes the next PUT answer 503 with
Retry-After: 7 (repeatable; consumed in order per method).

Usage:
  python3 tools/webdav_mock.py --port 8080 [--user tensile-hmi --password secret]
  python3 tools/webdav_mock.py --selftest      # protocol checks with http.client
  python3 tools/webdav_mock.py --replay        # also compile tools/tu_replay.c
                                               # against core/ and drive the C planner
"""
import argparse
import base64
import hashlib
import http.client
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
from email.utils import formatdate
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from xml.sax.saxutils import escape

DAV_PREFIX = "/remote.php/dav/files/"


class Store:
    def __init__(self, user, root="TensileOS"):
        self.lock = threading.Lock()
        self.user = user
        # key: decoded path relative to the user's files root, collections end in '/'
        self.nodes = {"": {"col": True, "mtime": time.time(), "etag": self._etag(b"root")},
                      root + "/": {"col": True, "mtime": time.time(), "etag": self._etag(b"r")}}
        self.inject = {}  # method -> list[(status, retry_after)]
        self.log = []

    @staticmethod
    def _etag(data):
        return hashlib.md5(data + str(time.time_ns()).encode()).hexdigest()[:13]

    def get(self, p):
        return self.nodes.get(p) or self.nodes.get(p + "/")

    def key(self, p):
        if p in self.nodes:
            return p
        if p + "/" in self.nodes:
            return p + "/"
        return None

    @staticmethod
    def parent(p):
        p = p.rstrip("/")
        return p.rsplit("/", 1)[0] + "/" if "/" in p else ""


def make_handler(store, password):
    class H(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"  # keep-alive

        def log_message(self, fmt, *args):  # quiet; keep a request log instead
            pass

        # ---- helpers
        def _send(self, status, body=b"", headers=None):
            self.send_response(status)
            hdrs = {"Content-Length": str(len(body))}
            if body:
                hdrs.setdefault("Content-Type", "application/xml; charset=utf-8")
            hdrs.update(headers or {})
            for k, v in hdrs.items():
                self.send_header(k, v)
            self.end_headers()
            if body:
                self.wfile.write(body)
            store.log.append((self.command, self.path, status))

        def _sabre_error(self, status, exc, msg):
            body = ('<?xml version="1.0" encoding="utf-8"?>\n<d:error xmlns:d="DAV:" xmlns:s="http://sabredav.org/ns">'
                    f'<s:exception>{exc}</s:exception><s:message>{escape(msg)}</s:message></d:error>').encode()
            self._send(status, body)

        def _body(self):
            n = int(self.headers.get("Content-Length") or 0)
            return self.rfile.read(n) if n else b""

        def _auth_ok(self):
            h = self.headers.get("Authorization", "")
            if h.startswith("Basic "):
                try:
                    u, _, p = base64.b64decode(h[6:]).decode().partition(":")
                    if u == store.user and p == password:
                        return True
                except Exception:
                    pass
            return False

        def _rel(self):
            path = urllib.parse.urlsplit(self.path).path
            pre = DAV_PREFIX + store.user + "/"
            if not (path + "/").startswith(pre):
                return None
            rel = urllib.parse.unquote(path[len(pre):]) if len(path) >= len(pre) else ""
            if any(seg in (".", "..") for seg in rel.split("/")):
                return None
            return rel

        def _pre(self):
            self._req_body = self._body()
            if not self._auth_ok():
                self._send(401, b"", {"WWW-Authenticate": 'Basic realm="Nextcloud", charset="UTF-8"'})
                return None
            inj = store.inject.get(self.command)
            if inj:
                st, ra = inj.pop(0)
                self._send(st, b"", {"Retry-After": str(ra)} if ra is not None else {})
                return None
            rel = self._rel()
            if rel is None:
                self._send(404)
                return None
            return rel

        # ---- methods
        def do_PROPFIND(self):
            rel = self._pre()
            if rel is None:
                return
            depth = self.headers.get("Depth", "1")
            if depth not in ("0", "1"):
                return self._sabre_error(403, "Sabre\\DAV\\Exception\\Forbidden", "Depth infinity not allowed")
            with store.lock:
                k = store.key(rel)
                if k is None:
                    return self._sabre_error(404, "Sabre\\DAV\\Exception\\NotFound", "File not found")
                keys = [k]
                if depth == "1" and store.nodes[k]["col"]:
                    keys += sorted(c for c in store.nodes if c != k and c.startswith(k) and
                                   "/" not in c[len(k):].rstrip("/"))
                out = ['<?xml version="1.0"?>\n<d:multistatus xmlns:d="DAV:" xmlns:s="http://sabredav.org/ns" '
                       'xmlns:oc="http://owncloud.org/ns" xmlns:nc="http://nextcloud.org/ns">']
                for c in keys:
                    n = store.nodes[c]
                    href = urllib.parse.quote(DAV_PREFIX + store.user + "/" + c)
                    lm = formatdate(n["mtime"], usegmt=True)
                    out.append(f"<d:response><d:href>{escape(href)}</d:href><d:propstat><d:prop>"
                               f"<d:getlastmodified>{lm}</d:getlastmodified>")
                    if n["col"]:
                        out.append("<d:resourcetype><d:collection/></d:resourcetype>")
                    else:
                        out.append(f"<d:getcontentlength>{len(n['data'])}</d:getcontentlength><d:resourcetype/>")
                    out.append(f"<d:getetag>&quot;{n['etag']}&quot;</d:getetag></d:prop>"
                               "<d:status>HTTP/1.1 200 OK</d:status></d:propstat>")
                    if n["col"]:
                        out.append("<d:propstat><d:prop><d:getcontentlength/></d:prop>"
                                   "<d:status>HTTP/1.1 404 Not Found</d:status></d:propstat>")
                    out.append("</d:response>")
                out.append("</d:multistatus>\n")
            self._send(207, "".join(out).encode())

        def do_MKCOL(self):
            rel = self._pre()
            if rel is None:
                return
            if self._req_body:
                return self._send(415)
            k = rel.rstrip("/") + "/"
            with store.lock:
                if store.key(rel.rstrip("/")) is not None:
                    return self._sabre_error(405, "Sabre\\DAV\\Exception\\MethodNotAllowed",
                                             "The resource you tried to create already exists")
                par = store.nodes.get(Store.parent(k))
                if not par or not par["col"]:
                    return self._sabre_error(409, "Sabre\\DAV\\Exception\\Conflict", "Parent node does not exist")
                store.nodes[k] = {"col": True, "mtime": time.time(), "etag": Store._etag(k.encode())}
            self._send(201)

        def do_PUT(self):
            rel = self._pre()
            if rel is None:
                return
            data = self._req_body
            with store.lock:
                par = store.nodes.get(Store.parent(rel))
                if not par or not par["col"]:
                    return self._sabre_error(409, "Sabre\\DAV\\Exception\\Conflict",
                                             "Files can only be created as children of collections")
                cur = store.nodes.get(rel)
                if cur and cur["col"]:
                    return self._send(405)
                inm = self.headers.get("If-None-Match")
                im = self.headers.get("If-Match")
                if inm and inm.strip() == "*" and cur:
                    return self._sabre_error(412, "Sabre\\DAV\\Exception\\PreconditionFailed",
                                             "An If-None-Match header was specified, but the ETag matched")
                if im and (not cur or im.strip() not in ("*", f'"{cur["etag"]}"')):
                    return self._sabre_error(412, "Sabre\\DAV\\Exception\\PreconditionFailed",
                                             "An If-Match header was specified, but none of the ETags matched")
                etag = Store._etag(data)
                store.nodes[rel] = {"col": False, "mtime": time.time(), "etag": etag, "data": data}
            self._send(204 if cur else 201, b"", {"ETag": f'"{etag}"', "OC-ETag": f'"{etag}"'})

    return H


def start(port, user, password):
    store = Store(user)
    srv = ThreadingHTTPServer(("127.0.0.1", port), make_handler(store, password))
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    return srv, store


# ---------------------------------------------------------------- self test
def selftest(replay):
    user, pw = "tensile-hmi", "Mock-App-Pass-1234"
    srv, store = start(0, user, pw)
    port = srv.server_address[1]
    base = f"{DAV_PREFIX}{user}/TensileOS/"
    auth = {"Authorization": "Basic " + base64.b64encode(f"{user}:{pw}".encode()).decode()}
    bad = {"Authorization": "Basic " + base64.b64encode(f"{user}:wrong".encode()).decode()}
    c = http.client.HTTPConnection("127.0.0.1", port)  # one keep-alive connection
    fails = 0

    def req(m, p, hdr=None, body=None):
        c.request(m, base + p, body=body, headers={**auth, **(hdr or {})})
        r = c.getresponse()
        return r.status, dict(r.getheaders()), r.read()

    def check(name, cond):
        nonlocal fails
        print(("ok   " if cond else "FAIL ") + name)
        fails += 0 if cond else 1

    check("PROPFIND base 207", req("PROPFIND", "", {"Depth": "0"})[0] == 207)
    check("MKCOL missing parent 409", req("MKCOL", "Tests/2026-10-14/")[0] == 409)
    check("MKCOL Tests 201", req("MKCOL", "Tests/")[0] == 201)
    check("MKCOL Tests again 405", req("MKCOL", "Tests/")[0] == 405)
    check("MKCOL date 201", req("MKCOL", "Tests/2026-10-14/")[0] == 201)
    st, h, _ = req("PUT", "Tests/2026-10-14/S%201.csv", {"If-None-Match": "*"}, b"a,b\n1,2\n")
    check("PUT new 201 + ETag", st == 201 and h.get("ETag", "").startswith('"'))
    etag = h.get("ETag")
    check("PUT existing If-None-Match:* 412",
          req("PUT", "Tests/2026-10-14/S%201.csv", {"If-None-Match": "*"}, b"x")[0] == 412)
    check("PUT If-Match stale 412", req("PUT", "Tests/2026-10-14/S%201.csv", {"If-Match": '"nope"'}, b"x")[0] == 412)
    check("PUT If-Match ok 204", req("PUT", "Tests/2026-10-14/S%201.csv", {"If-Match": etag}, b"y")[0] == 204)
    st, _, body = req("PROPFIND", "Tests/2026-10-14/", {"Depth": "1"})
    check("PROPFIND depth 1 lists file", st == 207 and b"S%201.csv" in body and b"&quot;" in body)
    check("PROPFIND missing 404", req("PROPFIND", "Nope/", {"Depth": "0"})[0] == 404)
    c.request("PROPFIND", base, headers={**bad, "Depth": "0"})
    r = c.getresponse()
    r.read()
    check("bad password 401", r.status == 401 and "WWW-Authenticate" in dict(r.getheaders()))
    store.inject["PUT"] = [(503, 7)]
    st, h, _ = req("PUT", "Tests/x.csv", {"If-None-Match": "*"}, b"z")
    check("injected 503 Retry-After", st == 503 and h.get("Retry-After") == "7")

    if replay:
        fails += run_replay(port, user, pw, store)
    srv.shutdown()
    print("selftest:", "PASS" if not fails else f"{fails} FAILED")
    return 1 if fails else 0


def run_replay(port, user, pw, store):
    here = os.path.dirname(os.path.abspath(__file__))
    core = os.path.join(here, "..", "components", "tensile_upload", "core")
    src = [os.path.join(here, "tu_replay.c")] + [os.path.join(core, f) for f in sorted(os.listdir(core))
                                                  if f.endswith(".c")]
    out_dir = tempfile.mkdtemp(prefix="tu_replay_")
    exe = os.path.join(out_dir, "tu_replay")
    cc = shutil.which("gcc") or shutil.which("cc")
    subprocess.check_call([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-D_POSIX_C_SOURCE=200809L",
                           "-I", core, "-o", exe] + src)
    local = os.path.join(out_dir, "S 2.csv")
    with open(local, "wb") as f:
        f.write(b"TensileCompanion,v1\n" + b"0.0,0.0\n" * 100)
    base = f"http://127.0.0.1:{port}{DAV_PREFIX}{user}/TensileOS/"
    fails = 0

    def run(name, password, remote, expect_rc, expect_text):
        nonlocal fails
        p = subprocess.run([exe, base, user, password, remote, local], capture_output=True, text=True)
        ok = p.returncode == expect_rc and expect_text in p.stdout
        print(("ok   " if ok else "FAIL ") + "replay: " + name)
        if not ok:
            print(p.stdout, p.stderr)
            fails += 1
        return p.stdout

    run("new day: MKCOL 201 -> PUT 201",
        pw, "Tests/2026-10-15/S 2.csv", 0, "DONE Tests/2026-10-15/S 2.csv created=1")
    run("same name again -> _TAB5", pw, "Tests/2026-10-15/S 2.csv", 0, "DONE Tests/2026-10-15/S 2_TAB5.csv")
    run("third time -> _TAB5_2", pw, "Tests/2026-10-15/S 2.csv", 0, "DONE Tests/2026-10-15/S 2_TAB5_2.csv")
    n_before = len(store.log)
    run("bad password -> AUTH_FAILED after one request", "wrong", "Tests/2026-10-15/S 3.csv", 2, "AUTH_FAILED")
    if len(store.log) - n_before != 1:
        print("FAIL replay: bad password sent", len(store.log) - n_before, "requests")
        fails += 1
    store.inject["PUT"] = [(503, 30)]
    run("503 -> RETRY_LATER with Retry-After", pw, "Tests/2026-10-15/S 4.csv", 3, "RETRY_LATER retry_after=30")
    out = run("listing parses", pw, "Tests/2026-10-15/S 5.csv", 0, "entries=")
    if out.count("file ") < 4:
        print("FAIL replay: expected >= 4 files in PROPFIND listing")
        fails += 1
    shutil.rmtree(out_dir, ignore_errors=True)
    return fails


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--user", default="tensile-hmi")
    ap.add_argument("--password", default=os.environ.get("TU_MOCK_PASSWORD", "Mock-App-Pass-1234"))
    ap.add_argument("--inject", action="append", default=[], help="METHOD:STATUS[:RETRY_AFTER], consumed once")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--replay", action="store_true", help="selftest + C planner replay (needs gcc)")
    a = ap.parse_args()
    if a.selftest or a.replay:
        sys.exit(selftest(a.replay))
    srv, store = start(a.port, a.user, a.password)
    for spec in a.inject:
        parts = spec.split(":")
        store.inject.setdefault(parts[0].upper(), []).append((int(parts[1]), parts[2] if len(parts) > 2 else None))
    print(f"WebDAV mock on http://127.0.0.1:{srv.server_address[1]}{DAV_PREFIX}{a.user}/TensileOS/ (Ctrl-C to stop)")
    try:
        while True:
            time.sleep(1)
            while store.log:
                print(*store.log.pop(0))
    except KeyboardInterrupt:
        srv.shutdown()


if __name__ == "__main__":
    main()

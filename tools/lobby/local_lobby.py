#!/usr/bin/env python3
"""A stand-in for the Cloudflare Worker (worker.js), same API, for tests on one PC: copies of the game started with
--instance N list their games at 127.0.0.N, which the real service refuses as private addresses, so this one takes any.
python tools/lobby/local_lobby.py [port, default 8787]; then lobby_url = "http://127.0.0.1:8787" in settings.toml"""
import re, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

HOST_TTL, SEARCH_TTL = 20.0, 15.0
hosts = {}      # (game, "ip:port") -> (ver, seen)
searchers = {}  # (game, "ip:port") -> (ver, seen)
lock = threading.Lock()
ADDR = re.compile(r'\d{1,3}(\.\d{1,3}){3}:\d{1,5}')


class Handler(BaseHTTPRequestHandler):
    def reply(self, status, body):
        data = body.encode()
        self.send_response(status)
        self.send_header('content-type', 'text/plain; charset=utf-8')
        self.send_header('content-length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def handle_any(self, method):
        url = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(url.query).items()}
        game, ver, seen = q.get('game', ''), q.get('ver', ''), self.client_address[0]
        if not re.fullmatch(r'[a-z0-9]{1,16}', game) or not ver:
            return self.reply(400, 'bad game\n')
        addr = q.get('addr', '')
        if addr and not ADDR.fullmatch(addr):
            return self.reply(400, 'bad addr\n')
        now = time.time()
        with lock:
            for table, ttl in ((hosts, HOST_TTL), (searchers, SEARCH_TTL)):
                for k in [k for k, (_, t) in table.items() if now - t > ttl]:
                    del table[k]
            if url.path == '/v1/list' and method == 'GET':
                if addr:
                    if (game, addr) not in searchers:
                        print(f'searcher {addr} ({ver})', flush=True)
                    searchers[(game, addr)] = (ver, now)
                out = f'you {seen}\n' + ''.join(f'host {a}\n' for (g, a), (v, _) in hosts.items() if g == game and v == ver)
                return self.reply(200, out)
            if not addr:
                return self.reply(400, 'bad addr\n')
            if url.path == '/v1/host' and method == 'POST':
                if (game, addr) not in hosts:
                    print(f'host {addr} ({ver})', flush=True)
                hosts[(game, addr)] = (ver, now)
                out = f'ok {seen}\n' + ''.join(f'searcher {a}\n' for (g, a), (v, _) in searchers.items()
                                               if g == game and v == ver and a != addr)
                return self.reply(200, out)
            if url.path == '/v1/unhost' and method == 'POST':
                hosts.pop((game, addr), None)
                print(f'unhost {addr}', flush=True)
                return self.reply(200, f'ok {seen}\n')
        self.reply(404, 'not found\n')

    def do_GET(self):
        self.handle_any('GET')

    def do_POST(self):
        self.handle_any('POST')

    def log_message(self, *args):
        pass


if __name__ == '__main__':
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8787
    print(f'local lobby on http://127.0.0.1:{port}', flush=True)
    ThreadingHTTPServer(('127.0.0.1', port), Handler).serve_forever()

// MechAssault PC online game list, a Cloudflare Worker (free plan). It only lists the System Link games people host
// over the internet: the matches themselves run between the players' PCs, so its traffic is a few small requests a
// minute per host. One Durable Object per game keeps the list in memory; a host that stops checking in drops off after
// kTtlMs, and an object Cloudflare unloads starts empty and refills from the next check-ins.
//
// API, plain text (the game reads it with a few lines of C++):
//   POST /v1/host?game=G&ver=V&addr=IP:PORT    list (or refresh) this game; answers "ok <your address>"
//   POST /v1/unhost?game=G&ver=V&addr=IP:PORT  take it off
//   GET  /v1/list?game=G&ver=V                 "you <your address>", then "host IP:PORT" per game
// addr is where the host's System Link socket is reached from the internet (its public IPv4 and the port its router
// maps, from STUN or UPnP). When the request itself arrives over IPv4, addr must carry that same address, so nobody can
// list someone else's machine.

const kTtlMs = 45000;
const kMaxHosts = 256;
const kAddr = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3}):(\d{1,5})$/;

function text(body, status = 200) {
    return new Response(body, {status, headers: {'content-type': 'text/plain; charset=utf-8', 'cache-control': 'no-store'}});
}

// a public IPv4 address and a port, or null
function parseAddr(s) {
    const m = kAddr.exec(s || '');
    if (!m) return null;
    const b = m.slice(1, 5).map(Number), port = Number(m[5]);
    if (b.some(x => x > 255) || port < 1 || port > 65535) return null;
    const priv = b[0] === 10 || b[0] === 127 || b[0] === 0 || b[0] >= 224 || (b[0] === 172 && (b[1] & 0xf0) === 16) ||
                 (b[0] === 192 && b[1] === 168) || (b[0] === 169 && b[1] === 254) || (b[0] === 100 && (b[1] & 0xc0) === 64);
    if (priv) return null;
    return {ip: b.join('.'), port, key: `${b.join('.')}:${port}`};
}

export class Lobby {
    constructor(state, env) {
        this.hosts = new Map();  // "ip:port" -> {ver, seen}
    }

    async fetch(request) {
        const url = new URL(request.url);
        const seen = request.headers.get('x-client-ip') || '';
        const ver = url.searchParams.get('ver') || '';
        const now = Date.now();
        for (const [k, h] of this.hosts) if (now - h.seen > kTtlMs) this.hosts.delete(k);
        if (url.pathname === '/v1/list') {
            let out = `you ${seen}\n`;
            for (const [k, h] of this.hosts) if (h.ver === ver) out += `host ${k}\n`;
            return text(out);
        }
        const addr = parseAddr(url.searchParams.get('addr'));
        if (!addr) return text('bad addr\n', 400);
        if (seen.includes('.') && seen !== addr.ip) return text('addr is not yours\n', 403);
        if (url.pathname === '/v1/unhost') {
            this.hosts.delete(addr.key);
            return text(`ok ${seen}\n`);
        }
        if (url.pathname === '/v1/host') {
            if (!this.hosts.has(addr.key) && this.hosts.size >= kMaxHosts) return text('full\n', 503);
            this.hosts.set(addr.key, {ver, seen: now});
            return text(`ok ${seen}\n`);
        }
        return text('not found\n', 404);
    }
}

export default {
    async fetch(request, env) {
        const url = new URL(request.url);
        const game = url.searchParams.get('game') || '';
        if (!/^[a-z0-9]{1,16}$/.test(game)) return text('bad game\n', 400);
        if (!url.searchParams.get('ver')) return text('bad ver\n', 400);
        const ok = (url.pathname === '/v1/list' && request.method === 'GET') ||
                   ((url.pathname === '/v1/host' || url.pathname === '/v1/unhost') && request.method === 'POST');
        if (!ok) return text('not found\n', 404);
        const headers = new Headers({'x-client-ip': request.headers.get('cf-connecting-ip') || ''});
        const stub = env.LOBBY.get(env.LOBBY.idFromName(game));
        return stub.fetch(new Request(url.toString(), {method: request.method, headers}));
    },
};

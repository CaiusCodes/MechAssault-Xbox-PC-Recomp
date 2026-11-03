# MechAssault PC online game list

The game's System Link menu finds games hosted over the internet through this small list service. It only lists
games: each host checks in every 5 seconds while it hosts and hears who is searching, and a player's System Link
search fetches the list. The game's own screens do the rest, as on a LAN. Matches run between the players' PCs; nothing
of the game goes through the service.

It runs on Cloudflare Workers' free plan (100,000 requests a day: enough for a few hosts all day, or many evening
sessions), with no server to rent or maintain.

## Put it online (once)

1. Make a free Cloudflare account at https://dash.cloudflare.com/sign-up (an email address; no card for the free plan).
2. Install Node.js (https://nodejs.org, the LTS installer) if this PC does not have it.
3. In this folder, run:

   ```
   npx wrangler login
   npx wrangler deploy
   ```

   `login` opens the browser to allow the deploy; `deploy` prints the service's address, such as
   `https://mechassault-lobby.<your-account>.workers.dev`.
4. Put that address in `kDefaultLobby` in `sdk/runtime/xdk/online.cpp` and rebuild, so every copy of the game uses
   it. (A player can also point their copy elsewhere with `lobby_url = "..."` in `resources\settings\settings.toml`.)

Cloudflare's dashboard shows the requests per day under Workers & Pages > mechassault-lobby.

## API

Plain text, described at the top of `worker.js`: `POST /v1/host`, `POST /v1/unhost` and `GET /v1/list`, each with
`game` and `ver`; games of another `ver` are not listed to each other.

## What players need

- **A router with UPnP on** (most home routers): the game opens its UDP ports 1000 to 1002 on the router while it hosts
  or searches, and closes them a minute after. MechAssault's host announces its game by broadcast rather than
  answering searches, so the host is told who is searching and sends its announcements to them too; a joiner's
  router has to let those in. Without UPnP, forward UDP 1000 to 1002 to the PC by hand. Windows Firewall asks once
  to let MechAssault through.
- A player on a strict network (some mobile and carrier connections) cannot host for internet players; virtual LAN
  (Radmin VPN, ZeroTier, Tailscale) still works for them.
- The switch is "Online games" on the setup screen (`online_games` in settings.toml), on by default. With it on, the
  game also asks a public STUN server (stun.l.google.com) for the PC's public address when the list service saw it
  over IPv6.

## Testing on one PC

`local_lobby.py` is a stand-in with the same API that also lists the loopback addresses of copies started with
`--instance N`:

```
python tools/lobby/local_lobby.py 8787
```

then `lobby_url = "http://127.0.0.1:8787"` in the dev build's settings.toml, and `XBE_NO_LAN=1` so the copies hear
each other's searches only through the list (scripts/run2.ps1 with the System Link pad scripts).

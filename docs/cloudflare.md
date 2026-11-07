# Public hosting at nfs4.xyz

Cloudflare terminates HTTPS and forwards requests through `cloudflared` to the local server at
`http://127.0.0.1:8084` (nfs3 keeps 8080). The tunnel carries the page, the WebAssembly build,
the lobby and the `/net` WebSocket relay. **No game files are served publicly:** the server runs
without `--game`, so every player picks their own NFS4 folder (with the Modern Patch beta) and
the page copies it into that browser's private storage. That's also why R2 isn't used.

## What is set up on this host

| | |
|---|---|
| Tunnel | `nfs4-web`, UUID `d2d5e212-767d-437c-9f3f-901e6b295452` |
| Config / credentials | `build/cloudflare/config.yml`, `build/cloudflare/credentials.json` (git-ignored, keep them) |
| Login for the nfs4.xyz zone | `build/cloudflare/home/.cloudflared/cert.pem` (separate from the nfs3 one in `~/.cloudflared`) |
| cloudflared | `build/bin/cloudflared`, official release 2026.10.7 (own copy; OpenSnitch needs an allow rule for this path) |

Nothing runs as a service; start both by hand, in two terminals, from the repository root:

```sh
node web/serve.mjs 8084
build/bin/cloudflared tunnel --no-autoupdate --config build/cloudflare/config.yml run
```

Restart the first after `./build-web.sh` or page changes.

## DNS (one time)

```sh
HOME=$PWD/build/cloudflare/home build/bin/cloudflared tunnel login     # pick nfs4.xyz in the browser
build/bin/cloudflared tunnel --origincert build/cloudflare/home/.cloudflared/cert.pem route dns nfs4-web nfs4.xyz
```

Or in the dashboard: DNS → add `CNAME nfs4.xyz → d2d5e212-767d-437c-9f3f-901e6b295452.cfargotunnel.com`, proxied.

## Checks

```sh
curl -sI https://nfs4.xyz/ | grep -i cross-origin          # COOP same-origin, COEP require-corp
curl -s -o /dev/null -w '%{http_code}\n' https://nfs4.xyz/dist/nfs4.wasm
curl -s -o /dev/null -w '%{http_code}\n' https://nfs4.xyz/__dev/game-list   # must be 404: no game files public
```

# Network between the dev PC, the container and the console

Every tool in `scripts/` and `tools/` talks to the console from inside the Docker
container: FTP to deploy and drive, the elfldr socket to launch and close, klog to
read the kernel log. All of it works on Docker Desktop's **default bridge network**
with no special configuration.

## Why bridge, and not host networking

On Linux `network_mode: host` puts a container on the machine's own network stack.
On Windows it does not: Docker Desktop runs containers inside a WSL2 virtual
machine, so "the host" from the container's point of view is that VM, not the
Windows PC. Host networking there adds confusion without adding reach. The bridge
already NATs outbound traffic onto the LAN, and the container is always the
**client** (the console is the server). So: no `network_mode: host`, no
`--privileged`, no `cap_add`.

## The one limitation, and what it breaks

Bridge networking does not let the console connect *into* the container. Anything
the console has to fetch from the dev PC must run on the **host**, not in the
container:

- `tools/provider-server.sh` (provider UI bundles, a test playlist and guide)
  refuses to start if it sees `/.dockerenv`. A server started inside would be
  invisible to the console even though `curl` from inside works, which looks exactly
  like a broken bundle.
- `tools/test-addon/serve.sh`, `tools/mock_xtream_server.py`,
  `tools/laptop_ftp_server.py` and the Emby/Jellyfin test servers likewise run on the
  host. Allow the first Windows Firewall prompt for **Private** networks.
- A quick playlist server: `python -m http.server <port> --bind <host LAN ip>`. The
  IPTV provider ignores port 8099 on purpose, so pick another.

If you really need a listener in the container, publish the port in the commented
`ports:` block of `docker-compose.yml`.

## Ports

| Port | Service | Used by |
|---|---|---|
| 2121 | `ps5-payload-ftpsrv` | deploy, the dev remote, log and screenshot pulls |
| 9021 | `ps5-payload-elfldr` | `launch`, `close` |
| 3232 | `ps5-payload-klogsrv` | `klog.sh`, `cycle` |
| 8080 | `ps5-payload-websrv` | `shot.sh`; read-only `/fs` |
| 9780 (default) | EVO's own log server | `/raw`, `/stats`, the phone input page, `evo-dash.py`. User-settable (1024–65535) in Settings → PHONE & LOG PORT with n+1..n+9 fallback; dev tools accept `PS5_LOG_PORT` |
| 9295 and discovery | Remote Play | [remote-play.md](remote-play.md) |

All of these except EVO's log server (port 9780 by default) need the jailbreak re-run after every console reboot.

## Setting `PS5_HOST`

The console's address is never committed. Put it in `.env` at the repo root
(`cp .env.example .env`); `docker compose` reads that file only, so a `.env.local`
on its own leaves the container pointing at the old address. Or per command:
`PS5_HOST=192.168.1.50 ./tools/evo-remote.sh status`. Find it under Settings →
Network → Connection Status. It moves after a power cut or a router change; if
every tool suddenly times out, check this first.

## Verifying connectivity

```bash
ping -c 3 "$PS5_HOST"
nc -vz "$PS5_HOST" 2121        # FTP
nc -vz "$PS5_HOST" 9021        # elfldr
```

## Troubleshooting

| Symptom | Cause |
|---|---|
| `ping` works, a port refuses | the jailbreak lapsed (re-run it, then start the payloads), or that service is not running |
| nothing resolves `ps5` | the SDK's default hostname relies on mDNS, which the container does not use. Use the IP |
| `ping` and the port both fail | not the same subnet or VLAN; AP/client isolation on a guest or wireless network; a third-party security suite. Test with `Test-NetConnection <ip> -Port 2121` from PowerShell; if that works and the container fails, restart Docker Desktop |
| `PS5_HOST=localhost` | that is the container itself. To reach something on Windows use `host.docker.internal` |
| a tool times out but the console is up | the far end holds the socket; a `timeout` or curl 28 on an `evo-remote.sh` call is a normal outcome ([hardware-cycle.md](hardware-cycle.md)) |
| EVO's DNS lookup fails | EVO resolves through the console's own resolver, so check the console's DNS setting; a DNS filter that answers `0.0.0.0` for a blocked name is logged as "blocked by the network's DNS" |

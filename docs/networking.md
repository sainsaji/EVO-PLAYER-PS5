# Docker → PS5 networking (Windows host)

The container must open a TCP connection to the console. That is the only
network requirement of the ELF workflow, and it works on Docker Desktop's
**default bridge network** with no special configuration.

## Why bridge, and why not host networking

On Linux, `network_mode: host` puts the container on the machine's own network
stack. **On Windows it does not do the equivalent thing.** Docker Desktop runs
containers inside a WSL2 (or Hyper-V) virtual machine, so "the host" from the
container's point of view is that VM, not your Windows PC. Enabling host
networking there attaches you to the VM's stack and does not hand you the
Windows LAN interface — it adds confusion without adding reach.

The default bridge already does what we need. Outbound traffic is NAT'd through
the VM onto your LAN, and the container is always the **client**: the PS5's
`ps5-payload-elfldr` is the server listening on 9021.

So: no `network_mode: host`, no `--privileged`, no `cap_add`. If a guide tells
you to use those for this, it is solving a different problem.

## The one limitation

Bridge networking does not let the PS5 initiate a connection *into* the
container. Nothing in the ELF workflow needs that. If you later run a listener
the console must reach, publish the port explicitly — there is a commented
`ports:` block in `docker-compose.yml`.

## Verifying connectivity

Inside the container:

```bash
# 1. Is the console up at all?
ping -c 3 "$PS5_HOST"

# 2. Is the ELF loader listening? This is the check that matters.
nc -vz "$PS5_HOST" 9021
```

A good result looks like:

```
Connection to 192.168.1.50 9021 port [tcp/*] succeeded!
```

`scripts/deploy.sh` runs this probe automatically before transferring, and
prints a diagnostic checklist if it fails.

## Setting PS5_HOST

The console's address is never committed. Pick one:

```bash
# Per command
PS5_HOST=192.168.1.50 ./scripts/deploy.sh output/elf/hello_world.elf

# For the shell session
export PS5_HOST=192.168.1.50

# Persistently for compose - create .env at the repo root (git-ignored)
echo "PS5_HOST=192.168.1.50" > .env
```

Find the address on the console: **Settings → Network → Connection Status**.

## Troubleshooting

### `ping` works, port 9021 refuses

The console is reachable but the loader is not running. Almost always this
means the jailbreak has lapsed — **it must be re-run after every reboot**, and
`ps5-payload-elfldr` must be started afterwards. Re-run the exploit and try
again.

### Nothing resolves the hostname `ps5`

The SDK defaults `PS5_HOST` to the literal hostname `ps5`, which relies on
mDNS/NetBIOS that the container does not participate in. Use the IP address.

### Both `ping` and the port fail

- Confirm the console and your PC are on the same subnet. A PC on Wi-Fi and a
  console on a different VLAN will not see each other.
- Many consumer routers enable **AP/client isolation** on the guest or wireless
  network, which blocks device-to-device traffic. Check the router.
- Windows Firewall does not normally block *outbound* container traffic, but
  third-party security suites sometimes do. Test from PowerShell first:
  ```powershell
  Test-NetConnection 192.168.1.50 -Port 9021
  ```
  If that succeeds from Windows but fails from the container, the problem is
  Docker Desktop, not your network — restart it, or toggle
  *Settings → Resources → Network*.

### `PS5_HOST=localhost` does nothing useful

`localhost` inside the container is the container. It is not your Windows
machine and certainly not the console. `deploy.sh` rejects this explicitly.
If you genuinely need to reach a service running on Windows, use
`host.docker.internal` (already mapped in `docker-compose.yml`).

### Deploy succeeds but nothing happens on screen

The payload ran and probably exited. Payload `stdout` goes nowhere unless the
loader redirects it. To see output:

- **Notifications** — the reliable channel. `evo_notify()` in
  `projects/common` uses `sceKernelSendNotificationRequest`, and every sample
  in this repo calls it.
- **klog** — run `ps5-payload-klogsrv` on the console and
  `nc $PS5_HOST 3232` from the container.
- **websrv** — launching through
  [`ps5-payload-websrv`](https://github.com/ps5-payload-dev/websrv) does
  redirect stdio.

### Slow builds rather than a network problem

If builds crawl, it is the Windows bind mount, not the network. Heavy object
trees are already kept on native Linux volumes (`/build/ffmpeg`, `/ccache`) for
exactly this reason. Give Docker Desktop more CPU and RAM under
*Settings → Resources*, and make sure the WSL2 backend is enabled.

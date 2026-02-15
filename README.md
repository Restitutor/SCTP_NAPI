# SCTP_NAPI

Opinionated native Node.js addon for SCTP SEQPACKET sockets. Provides a single `open()` call that returns a handle with `send()` and `close()` methods, with event-driven receive via libuv.

## System requirements

**Linux only.** SCTP is a kernel protocol — this addon links against `libsctp` and requires the kernel module loaded.

### 1. Install lksctp-tools

This provides the `libsctp.so` shared library and `/usr/include/netinet/sctp.h` header.

**Arch Linux:**
```bash
sudo pacman -S lksctp-tools
```

**Fedora / RHEL:**
```bash
sudo dnf install lksctp-tools-devel
```

### 2. Load the kernel module

The SCTP kernel module must be loaded. It usually auto-loads on first use, but to be explicit:

```bash
sudo modprobe sctp
```

### 3. Build tools

`node-gyp` compiles the C addon. It needs Python 3, `make`, and a C compiler:

```bash
# Arch
sudo pacman -S base-devel python

# Debian / Ubuntu
sudo apt install build-essential python3
```

## Setup

```bash
pnpm install
pnpx node-gyp rebuild
```

`pnpm install` pulls in the `node-gyp` dependency. `node-gyp rebuild` compiles `sctp_dgram.c` into `build/Release/sctp_dgram.node`.

## API

The addon exports a single function:

```javascript
const sctp = require("./build/Release/sctp_dgram.node");

const handle = sctp.open(bindHost, bindPort, recvMaxlen, onRecv);
```

### `sctp.open(bindHost, bindPort, recvMaxlen, onRecv)`

Creates an SCTP SEQPACKET socket, binds it, and starts listening for incoming messages. Returns a handle object.

- **bindHost** `string` — IP to bind to (`"0.0.0.0"` for all interfaces)
- **bindPort** `number` — port to bind to (`0` for ephemeral)
- **recvMaxlen** `number` — max bytes per received message (pre-allocated buffer)
- **onRecv** `(result: [Buffer, string, number] | string) => void` — called on each received message. `result` is `[data, host, port]`, or an error string on failure.

The socket is created with these options hardcoded:
- `SOCK_NONBLOCK` — non-blocking I/O
- `SO_REUSEADDR` — allow port reuse
- `SCTP_NODELAY` — disable Nagle's algorithm
- `SCTP_RTOINFO` — retransmission tuned for LAN (initial=250ms, min=200ms, max=5000ms)
- `SCTP_RECVRCVINFO` — enables receive info on incoming messages
- IPv6 dual-stack with `IPV6_V6ONLY=0` when binding to IPv6
- Sends use `SCTP_UNORDERED` flag and PR-SCTP TTL of 30s

### `handle.send(buffer, host, port)`

Send a message to the given address. Returns bytes sent.

- **buffer** `Buffer` — data to send
- **host** `string` — destination IP
- **port** `number` — destination port

### `handle.close()`

Stops receiving, closes the socket, and frees resources. Safe to call multiple times. If not called explicitly, the GC finalizer will clean up.

## Internals

Receive is event-driven via `uv_poll_t` on the socket fd, integrated into Node's event loop. When the fd becomes readable, the C callback drains up to 64 messages per tick (preventing event loop starvation), then yields. Each received message is copied into a JS-owned Buffer and delivered via the callback.

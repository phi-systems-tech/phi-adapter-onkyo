# phi-adapter-onkyo

## Overview

Integrates compatible Onkyo/Pioneer network receivers with phi-core.

## Supported Devices / Systems

- Onkyo/Pioneer network-capable AV receivers supported by the adapter protocol implementation

## Cloud Functionality

- Cloud required: `no`
- Local network integration only

## Known Issues

- Behavior may differ between receiver model lines and firmware versions.

## License

See `LICENSE`.

---

## Developer Documentation

### Purpose

Provides local network command/control integration for Onkyo/Pioneer receivers.

### Features

- Device communication over LAN
- IPC sidecar executable using `phi-adapter-sdk`, with no Qt dependency
- Descriptor-driven config schema (`configSchema`) sent during bootstrap
- Factory action `probe` (`Test connection`) handled via IPC
- Instance actions `settings` and `probeCurrentInput`

### Runtime Requirements

- phi-core with IPC adapter runtime enabled
- Network access to receiver endpoints

### Build Requirements

- `cmake`, `ninja-build`
- `phi-adapter-sdk` >= 0.9.0 (local checkout in `../phi-adapter-sdk` or installed package)
- `nlohmann-json3-dev`
- No Qt. The event loop, timers and string helpers come from `phi-runtime`
  through the SDK; JSON is nlohmann, which is what phi-core uses.

### Layout

One concern per translation unit, and everything above the connection is pure
enough to test without a receiver:

| File | What it is about |
| --- | --- |
| `onkyo_frames` | the eISCP wire, both directions: encode, scan, and read a status line |
| `onkyo_selector` | what an SLI code is: normalizing, labelling, deduplicating |
| `onkyo_endpoint` | addressing: ports, address literals, model names, probe candidates |
| `onkyo_iscp_client` | the connection: connect, send, read, on the loop and never blocking it |
| `onkyo_queue` | the order work happens in, and the coalescing rules |
| `onkyo_state` | what the receiver last said, and whether saying it again is news |
| `onkyo_inputs` | the input catalogue: which codes exist and what each is called |
| `onkyo_devicemodel` | the device and channel descriptors phi-core is given |
| `onkyo_schema` | adapter identity and the config schema phi-ui renders |
| `onkyo_instance` | the lifecycle that ties them together |
| `onkyo_probe` | the factory's blocking reachability check |
| `onkyo_json`, `onkyo_trace` | JSON accessors and the stderr diagnostic channel |

### Configuration

- No dedicated config file in this repository
- Device settings are configured through phi-core
- Factory scope fields:
  - `host`
  - `iscpPort` (ISCP port, typically `60128`)
  - `pollIntervalMs`
  - `retryIntervalMs`
- Instance scope fields:
  - `volumeMaxRaw`
  - `activeSliCodes`
  - `currentInputCode` (read-only helper, populated by `probeCurrentInput`)

An SLI code is two hexadecimal digits, which is what a receiver both accepts
and reports. Anything else is refused rather than sent: a code the receiver
cannot answer to produced a success nothing had confirmed.

### Runtime State Machine

One sidecar process, one execution thread per instance, and one event loop on
that thread. Adapter callbacks, timers and socket readiness all arrive on it,
one at a time.

- `Idle`
  - `start()` has run but no configuration has arrived. Nothing is polled,
    because there is no endpoint yet.

- `Running + Disconnected`
  - `connected=false`
  - Poll timer uses `retryIntervalMs`.
  - Three consecutive polls that could not open a connection are what turns
    connectivity off; one missed answer is not a receiver that has gone away.

**What connectivity means here:** the TCP connection, and only that. A receiver
in standby accepts the socket and hangs up without answering a query - it is
reported as connected, because a receiver you can reach is a receiver you can
switch on, and the UI has to offer that. Power stays `Unknown` until the
receiver says otherwise. Only a connection that cannot be made at all is
reported as disconnected.

- `Running + Connected`
  - `connected=true`
  - Poll timer uses `pollIntervalMs`.
  - One poll opens a connection and asks `PWRQSTN`, `MVLQSTN`, `AMTQSTN`,
    `SLIQSTN`, each awaited before the next goes out.
  - Channel updates are emitted only on value changes.

- `Paused`
  - phi-core disconnected. Polling stops and everything queued is answered, but
    the instance is not finished: phi-core reconnects and re-sends the
    configuration, which starts it again.

- `Stopped`
  - Terminal. Everything belonging to the loop is released here, because the
    destructor runs on the host thread and touching a timer from there is a
    thread-affinity error.

- `Work order`
  - A write drops any queued poll: a poll that has not started yet is about to
    be out of date anyway.
  - A second volume write replaces the first rather than queueing behind it.
    The one it replaced is answered - phi-core is waiting on every command id.
  - `probeCurrentInput` goes to the front, because somebody is watching a form
    field for it.
  - Nothing blocks the loop, so a write that arrives during a poll is queued
    the moment it arrives rather than after the poll finishes.

- `Power state`
  - `Unknown | Off | On`, from the receiver's own answers.
  - A write to any other channel while it is `Unknown` runs a poll first and
    then continues, rather than failing.
  - Forgotten when connectivity is lost: what was true of the old connection
    says nothing about the new one.

- `Channel invoke`
  - A successful non-power write schedules a refresh poll a second later, so
    what the receiver actually did is what ends up reported.

### Build

```bash
cmake -S . -B ../build/phi-adapter-onkyo/release-ninja -G Ninja
cmake --build ../build/phi-adapter-onkyo/release-ninja --parallel
ctest --test-dir ../build/phi-adapter-onkyo/release-ninja --output-on-failure
```

### Installation

- Build output: `../build/phi-adapter-onkyo/release-ninja/plugins/adapters/phi_adapter_onkyo_ipc`
- Deploy to: `/usr/lib/<triplet>/phi/plugins/adapters/`

### Known Rough Edges

- The config schema is generated for all 256 selector codes, so that a user can
  select and name an input this adapter has never heard of. For the 25 codes
  the shipped config knows, that is 259 instance fields and about 58 KB of
  JSON, sent at every bootstrap.
- A configured hostname is resolved with `getaddrinfo` on the loop thread when
  phi-core has not yet handed down an address. It is brief and it is rare - core
  resolves in the background and pushes the result - but it is a blocking call
  on an otherwise non-blocking path.

### Troubleshooting

- Error: device does not respond
- Cause: wrong endpoint or network reachability issues
- Fix: validate host/port and connectivity from phi-core host
- Symptom: discovery resolves to HTTP port (`80`) instead of ISCP
- Fix: set `iscpPort` explicitly in adapter config (for example `60128`)

### v1 Contract Notes

- Factory action `probe` reads only explicit action/form params (`host`, `ip`, `iscpPort`).
- No legacy nested `factoryAdapter` parameter fallback is used.
- Runtime device identity uses `deviceUuid` / adapter `externalId`; legacy `uuid` fallback is not used.

### Maintainers

- Phi Systems Tech team

### Issue Tracker

- https://github.com/phi-systems-tech/phi-adapter-onkyo/issues

### Releases / Changelog

- https://github.com/phi-systems-tech/phi-adapter-onkyo/releases
- https://github.com/phi-systems-tech/phi-adapter-onkyo/tags

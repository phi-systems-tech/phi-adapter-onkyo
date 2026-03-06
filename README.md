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
- IPC sidecar executable using `phi-adapter-sdk`
- Descriptor-driven config schema (`configSchema`) sent during bootstrap
- Factory action `probe` (`Test connection`) handled via IPC
- Instance actions `settings` and `probeCurrentInput`

### Runtime Requirements

- phi-core with IPC adapter runtime enabled
- Network access to receiver endpoints

### Build Requirements

- `cmake`
- Qt6 modules: `Core`, `Network`
- `phi-adapter-sdk` (local checkout in `../phi-adapter-sdk` or installed package)

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

### Runtime State Machine

The adapter runs as a single sidecar process with one worker thread per instance.

- `Stopped`
  - Instance is not running.
  - No polling is active.
  - Pending queued operations are flushed with failure.

- `Running + Disconnected`
  - `connected=false`
  - Poll timer uses `retryIntervalMs`.
  - Poll tries to establish TCP connectivity and query receiver state.
  - After repeated connect failures, connectivity remains disconnected.

- `Running + Connected`
  - `connected=true`
  - Poll timer uses `pollIntervalMs`.
  - Poll queries `PWRQSTN`, `MVLQSTN`, `AMTQSTN`, `SLIQSTN` in one session.
  - Channel updates are emitted only on value changes (deduped).

- `Poll preemption`
  - Poll is background work.
  - If prioritized work is queued (`channel invoke` or instance action), poll exits early.
  - This keeps write/actions responsive.

- `Power state`
  - Internal power cache: `Unknown | Off | On`.
  - Updated from ISCP responses.
  - Reset to `Unknown` when connectivity is lost.

- `Channel invoke`
  - Enqueued with priority over poll.
  - Successful non-power write schedules a near-term refresh poll.
  - Volume writes are coalesced in queue.

- `probeCurrentInput` action
  - If probe was requested while poll was already running:
    - No extra TCP query is started.
    - Result is returned from the last poll-resolved input code.
  - If poll was not running:
    - Explicit `SLIQSTN` query is sent (with retry).
  - On success:
    - `activeSliCodes` is patched with discovered code.
    - Missing/default input label can be patched from configured defaults.
    - Action result returns updated form values/choices and requests layout reload.

### Build

```bash
cmake -S . -B build
cmake --build build --parallel
```

### Installation

- Build output: `build/plugins/adapters/phi_adapter_onkyo_ipc`
- Deploy to: `/opt/phi/plugins/adapters/`

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

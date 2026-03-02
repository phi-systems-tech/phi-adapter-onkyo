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

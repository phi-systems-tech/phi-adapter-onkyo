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
- Logging category `phi-core.adapters.onkyo`

### Runtime Requirements

- phi-core with plugin loading enabled
- Network access to receiver endpoints

### Build Requirements

- `cmake`
- Qt6 modules: `Core`, `Network`
- `phi-adapter-api` (local checkout or installed package)

### Configuration

- No dedicated config file in this repository
- Device settings are configured through phi-core

### Build

```bash
cmake -S . -B build
cmake --build build --parallel
```

### Installation

- Build output: `build/plugins/adapters/libphi_adapter_onkyo.so`
- Deploy to: `/opt/phi/plugins/adapters/`

### Troubleshooting

- Error: device does not respond
- Cause: wrong endpoint or network reachability issues
- Fix: validate host/port and connectivity from phi-core host

### Maintainers

- Phi Systems Tech team

### Issue Tracker

- https://github.com/phi-systems-tech/phi-adapter-onkyo/issues

### Releases / Changelog

- https://github.com/phi-systems-tech/phi-adapter-onkyo/releases
- https://github.com/phi-systems-tech/phi-adapter-onkyo/tags

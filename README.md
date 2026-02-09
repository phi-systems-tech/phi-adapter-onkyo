# phi-adapter-onkyo

Standalone Onkyo / Pioneer adapter plugin for Phi.

## Build

```bash
cmake -S . -B build
cmake --build build --parallel
```

## Notes

- Expects `phi-adapter-api` as sibling directory (`../phi-adapter-api`) by default.
- Alternatively install `phi-adapter-api` and provide `CMAKE_PREFIX_PATH`.

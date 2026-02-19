<p align="center">
  <img src="assets/ammio.png" alt="ammio" width="120"/>
</p>

<p align="center">
  <strong>Environment simulator for system-level testing of safety-critical software over various communication protocols.</strong>
</p>

# ammio

**ammio** is a C-based interface layer for testing critical software systems. It acts as a bridge between test scripts and the System Under Test (SUT), handling protocol communication and signal management.

For the SUT, ammio simulates the outside world. For the tester, ammio provides an interface to force inputs and observe outputs.

## Quickstart

### Requirements

- CMake 3.13+
- MSVC 2022 (Windows)
- Git submodules initialized

### Build

```bash
git clone https://github.com/ammonit-software/ammio.git
git submodule update --init --recursive
scripts\build.bat
```

### Run

Use the convenience script:

```bash
scripts\run.bat
```

Or run it directly in cmd:

```bash
build\Debug\ammio.exe config\config.json config\interface.json
```

### Test interface

ammio exposes a REQ/REP socket on a given endpoint. Send JSON requests:

```json
{"cmd": "write", "name": "IO_BRAKE_ENABLE", "value": 1}
{"cmd": "read",  "name": "SYSTEM_STATUS"}
{"cmd": "list_vars"}
{"cmd": "list_errors"}
```

## Architecture

```
┌─────────────────────────┐
│  client (e.g. ammtest)  │
└─────────┬───────────────┘
          │ JSON REQ/REP
          │
┌─────────│───────────────────────────────────────────────────┐
│         │                                       ammio (C)   │
│         ▼                                                   │
│  ┌─────────────┐  ┌─────────────┐  ┌──────────────────────┐ │
│  │ var_server  │◄─│  var_table  │◄─│  Protocol Plugins    │ │
│  │             │  │ (hash map)  │  │  (TRDP, CAN, ...)    │ │
│  └─────────────┘  └─────────────┘  └──────────┬───────────┘ │
└───────────────────────────────────────────────┼─────────────┘
                                                │ Protocol bus
                                                ▼
                                    ┌─────────────────────────┐
                                    │  System Under Test      │
                                    └─────────────────────────┘
```

**var_server**: Handles client communication. Parses JSON requests, enforces direction constraints, and returns numeric error codes.

**var_table**: Thread-safe hash map storing all signals with their type, direction (`input`/`output`), value, and timestamp. Inputs can be written and read by the client. Outputs are read-only — only the protocol layer can update them.

**Protocol plugins**: Each plugin implements `init / start / stop`. Internally runs a single `thread_process` loop: publish inputs to the protocol bus, run protocol housekeeping (`select` + `process`), receive outputs from the protocol bus. All protocol operations run in one thread — protocol libraries are not thread-safe per session.

## Configuration

**`config/config.json`** — application settings:

```json
{
    "log_level": 0,
    "ammio_endpoint": "tcp://127.0.0.1:5555"
}
```

**`config/interface.json`** — protocol interface definition:

```json
{
    "trdp": {
        "local_ip": "10.0.0.1",
        "containers": {
            "inputs": [...],
            "outputs": [...]
        }
    }
}
```

Inputs = signals ammio sends to the SUT (test client can write).
Outputs = signals ammio receives from the SUT (test client can only read).

## Related Projects

- **[ammtest](https://github.com/ammonit-software/ammtest)**: Python test framework that communicates with ammio over the REQ/REP interface, and supports test scripts generation, automation and management.
- **[Ammonit Software](https://github.com/ammonit-software)**: Parent organization.

## License

This project is open source and available under the [MIT License](LICENSE).
<p align="center">
  <img src="assets/ammio.png" alt="ammio" width="350"/>
</p>

<p align="center">
  <strong>Environment simulator for system-level testing of safety-critical software over various communication protocols.</strong>
</p>

# ammio

**ammio** is a service that bridges test clients and a System Under Test (SUT) over its native communication protocol. For the SUT, it simulates the outside world. For the tester, it exposes all SUT variables over a simple JSON API — no protocol knowledge required.

It addresses two core pain points in critical systems testing:

- *The protocol barrier*: SUTs in critical systems speak industry-specific protocols — TRDP, CAN, ARINC 429, OPC-UA, etc. Without **ammio**, a tester needs deep knowledge of those protocols just to set a single variable. **ammio** abstracts the entire SUT interface and exposes it in real time over a plain JSON API.
- *The feedback loop speed*: Setting up a hardware-in-the-loop bench for a critical system can take days. **ammio** lets you interact with the SUT programmatically without a physical bench — scripts, experiments, and automated tests can run immediately.

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

Once **ammio** is running, interact with it by sending JSON requests over nng REQ/REP (TCP, configured via `ammio_endpoint`).

**Write an input to the SUT** — force a variable value onto the protocol bus. The SUT reads it as if it came from the real environment. Useful for bringing the SUT to a desired state.

```json
{"cmd": "write", "name": "IO_BRAKE_ENABLE", "value": 1}
```
```json
{"status": "ok"}
```

**Read an output from the SUT** — observe a variable that the SUT is publishing onto the protocol bus. Useful for checking the SUT's behaviour in response to inputs.

```json
{"cmd": "read", "name": "SYSTEM_STATUS"}
```
```json
{"name": "SYSTEM_STATUS", "type": "uint8", "dir": "output", "value": 3, "timestamp": 1740000000000}
```

## Architecture

**ammio** sits between an *ammio client* and the SUT — speaking the SUT's native protocol on one side, and exposing a simple JSON API on the other. This makes the SUT accessible to any tooling, agnostic from the underlying communication layer.

All *ammio clients* communicate over [nng](https://nng.nanomsg.org/) REQ/REP. **[ammtest](https://github.com/ammonit-software/ammtest)**, also built by Ammonit Software, is the recommended client — it adds test scripting, automation, and reporting on top of ammio.

In that way, anybody is able to interact with the SUT programmatically — tests can be scripted and automated, experiments launched, and custom integration built.

There are two variable directions:

- **Inputs** — the client writes a value, **ammio** publishes it to the SUT over the protocol bus
- **Outputs** — the SUT publishes a value, **ammio** receives it and makes it available for the client to read

All variables live in an internal store (**var_table**) that is continuously synchronized with the protocol bus. Any nng-compatible client can connect and interact with those variables at any time.

```
  test client (e.g. ammtest)
         │
         │  write(INPUT_VAR, value)      → ammio forwards to SUT via protocol bus
         │  value = read(OUTPUT_VAR)     ← ammio receives from SUT via protocol bus
         │
         │ nng REQ/REP · JSON · TCP
         │
  ┌──────│───────────────────────────────────────────────────┐
  │      │                                          ammio    │
  │      ▼                                                   │
  │  ┌────────────┐   ┌──────────────────┐   ┌───────────┐   │
  │  │ var_server │◄──│    var_table     │◄──│Interfaces │   │
  │  │            │   │ inputs / outputs │   │TRDP · CAN │   │
  │  └────────────┘   └──────────────────┘   └─────┬─────┘   │
  └─────────────────────────────────────────────── │ ────────┘
                                                   │ protocol bus
                                                   ▼
                                          System Under Test (SUT)
```

**var_server**: Client-facing endpoint (nng REQ/REP over TCP). Accepts JSON requests, enforces direction constraints (outputs are read-only for clients), and returns numeric error codes.

**var_table**: Central variable store. Holds every variable with its type, direction (`input`/`output`), current value, and timestamp. Continuously updated by the interface layer. Thread-safe — clients and interfaces access it concurrently without coordination.

**Interfaces**: Each interface (TRDP, CAN, ...) implements `init / start / stop` and runs a single `thread_process` loop — push inputs to the protocol bus, run protocol housekeeping, pull outputs back into var_table. One thread per interface keeps protocol libraries that are not thread-safe from conflicting.

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

- Inputs = variables **ammio** sends to the SUT (test client can write).
- Outputs = variables **ammio** receives from the SUT (test client can only read).

## Related Projects

- **[ammtest](https://github.com/ammonit-software/ammtest)**: Python test framework that communicates with **ammio** over the REQ/REP interface, and supports test scripts generation, automation and management.
- **[Ammonit Software](https://github.com/ammonit-software)**: Parent organization.

## License

This project is open source and available under the [MIT License](LICENSE).
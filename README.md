<p align="center">
  <img src="assets/ammio.png" alt="ammio" width="350"/>
</p>

<p align="center">
  <strong>Protocol agnostic test interface for critical-software systems. Talk to any System Under Test (SUT) in JSON, regardless of its protocol.</strong>
</p>

# ammio

**ammio** sits between your test tooling and your SUT. It speaks the SUT's protocol (Modbus, OPC UA, TRDP) on one side and exposes every variable through one small JSON API on the other.

- **No protocol barrier**: read and write SUT variables by name, without knowing the protocol.
- **Fast feedback**: scripts and automated tests talk to the SUT directly, without a hardware bench.
- **Protocol changes don't break tests**: when the SUT changes protocol, only the config changes, and the JSON API stays the same.

## Quickstart

**1. Download** the latest release for your system:

```bash
curl -L https://github.com/ammonit-software/ammio/releases/latest/download/ammio-linux-x64.tar.gz | tar -xz
```

On Windows, download `ammio-windows-x64.zip` from the same release page and unzip it.

Both archives contain the `ammio` executable and one example config per protocol.

**2. Describe your SUT** in one JSON file. Start from an example, e.g. `cp modbus.example.json sut.json`, and edit it:

```json
{
    "plc": {
        "protocol": "modbus",
        "host": "192.168.1.10",
        "inputs":  [ { "name": "motor_speed_actual",   "address": 0,  "type": "uint16" } ],
        "outputs": [ { "name": "motor_speed_setpoint", "address": 10, "type": "uint16" } ]
    }
}
```

**3. Check and run**:

```bash
./ammio --check sut.json
```

```bash
./ammio sut.json
```

`--check` validates the config without connecting and reports every problem it finds, each with its exact location (for example `plc.inputs[2].type: unknown type 'unit16'`).

**4. Talk to it** from any language with an [nng](https://nng.nanomsg.org/) binding. With Python (`pip install pynng`):

```python
import json, pynng

with pynng.Req0(dial="tcp://127.0.0.1:5555") as sock:
    def ammio(**request):
        sock.send(json.dumps(request).encode())
        return json.loads(sock.recv())

    ammio(cmd="write", name="motor_speed_actual", value=1200)
    print(ammio(cmd="read", name="motor_speed_setpoint"))
```

**[ammtest](https://github.com/ammonit-software/ammtest)** is the recommended client. It adds test scripting, pytest integration and reporting on top of ammio.

## Command line

```
ammio [options] <config.json>

  -e, --endpoint <url>  where clients connect (default: tcp://127.0.0.1:5555)
  -c, --check           validate the config and exit
  -v, --verbose         log debug details
  -h, --help            show this help
      --version         show the version
```

ammio stops cleanly on Ctrl+C or `SIGTERM`.

## Client API

Clients send JSON requests over nng REQ/REP (TCP by default). Every request gets exactly one JSON reply.

| Request | Reply |
|---|---|
| `{"cmd": "read", "name": "x"}` | `{"name": "x", "type": "uint16", "dir": "output", "value": 42, "timestamp": 1732460000123456789}` |
| `{"cmd": "write", "name": "x", "value": 42}` | `{"status": "ok"}` |
| `{"cmd": "list_vars"}` | `{"vars": [{"name": "x", "type": "uint16", "dir": "output"}, ...]}` |

- `value` is a JSON number, or `true`/`false` for `bool` variables (numbers 0/1 are accepted too).
- `timestamp` is the time of the last update, in nanoseconds since the Unix epoch: the last client write for inputs, the last value received from the SUT for outputs. `0` means never updated. Use it to detect stale data.
- Writes are exact. A value that doesn't fit the variable's type (out of range, or a fraction for an integer type) is rejected, never truncated.

Errors are returned as `{"error": <code>, "message": "<what went wrong>"}`:

| Code | Meaning |
|---|---|
| 1 | Unknown variable |
| 2 | Variable is an output (read-only) |
| 3 | Request is not a JSON object |
| 4 | Unknown command, or missing `cmd` / `name` |
| 5 | `write` without `value` |
| 6 | Value doesn't fit the variable's type |

## Configuration

A config file is a JSON object of **connections**. Each connection has a user-chosen name, a `protocol`, the protocol's connection settings, and lists of `inputs` and `outputs`. A config can hold any number of connections of any protocols. Variable names must be unique across the whole file.

| Direction | Data flow | Typical variables |
|---|---|---|
| `inputs` | client → ammio → SUT | Sensor readings, external device states, feedback the SUT consumes |
| `outputs` | SUT → ammio → client | Commands, setpoints, enable signals the SUT produces (read-only for clients) |

**Types**: `bool`, `uint8`, `int8`, `uint16`, `int16`, `uint32`, `int32`, `float32`, `float64`.

ammio is strict: unknown keys, missing keys, bad values, duplicate names and overlapping addresses are all errors. A typo such as `"adress"` stops startup instead of silently becoming address 0.

Ready-to-use examples are in [`config/`](config).

### Modbus

ammio is the Modbus master (client). It writes inputs to holding registers and reads outputs from holding registers, once per `period_ms`.

```json
{
    "plc": {
        "protocol": "modbus",
        "host": "192.168.1.10",
        "inputs":  [ { "name": "motor_current", "address": 2,  "type": "float32" } ],
        "outputs": [ { "name": "motor_enable",  "address": 14, "type": "bool" } ]
    }
}
```

| Key | Default | Description |
|---|---|---|
| `host` | | Modbus TCP: IP address or hostname of the slave |
| `port` | `502` | Modbus TCP port |
| `device` | | Modbus RTU: serial port (`COM3`, `/dev/ttyUSB0`); use instead of `host` |
| `baud`, `parity`, `data_bits`, `stop_bits` | `9600`, `"N"`, `8`, `1` | Modbus RTU serial settings (parity: `"N"`, `"E"` or `"O"`) |
| `slave_id` | `1` | Unit identifier |
| `period_ms` | `100` | Read/write cycle |
| `word_order` | `"big"` | Word order of 32/64-bit values: `"big"` = high word first (standard), `"little"` = low word first |

Variables: `name`, `address` (0-based register), `type`. 8- and 16-bit types use one register, 32-bit types use two, `float64` uses four.

### OPC UA

ammio is an OPC UA client. Each period it writes all inputs in one request and reads all outputs in one request.

```json
{
    "scada": {
        "protocol": "opcua",
        "endpoint": "opc.tcp://192.168.1.20:4840",
        "inputs":  [ { "name": "motor_speed_actual", "node_id": "ns=1;s=Motor.SpeedActual", "type": "float32" } ],
        "outputs": [ { "name": "motor_enable",       "node_id": "ns=1;i=1001",              "type": "bool" } ]
    }
}
```

| Key | Default | Description |
|---|---|---|
| `endpoint` | | Server URL |
| `username`, `password` | | Optional, set both or neither |
| `period_ms` | `100` | Read/write cycle |

Variables: `name`, `node_id` (`ns=1;s=Name`, `ns=1;i=1001`, ... use a browser such as UaExpert to find them), `type`. The type must match the node's data type (`bool` = Boolean, `uint8` = Byte, `float32` = Float, `float64` = Double, ...). A mismatch is reported and the value is never misread.

### TRDP

ammio publishes inputs and subscribes to outputs as TRDP process data telegrams.

```json
{
    "train": {
        "protocol": "trdp",
        "local_ip": "192.168.1.100",
        "inputs": [
            {
                "name": "DOOR_STATUS", "comid": 2000, "dest_ip": "239.255.1.2", "period_ms": 100, "size": 4,
                "variables": [
                    { "name": "door_is_open", "offset": 0, "type": "uint8" },
                    { "name": "door_alarm",   "offset": 3, "type": "bitset8", "bits": ["obstacle", "emergency_release"] }
                ]
            }
        ],
        "outputs": [
            {
                "name": "DOOR_CONTROL", "comid": 1000, "dest_ip": "239.255.1.1", "period_ms": 100, "size": 4,
                "variables": [ { "name": "door_open_cmd", "offset": 0, "type": "uint8" } ]
            }
        ]
    }
}
```

| Key | Default | Description |
|---|---|---|
| `local_ip` | `"0.0.0.0"` | Local interface to use. Loopback (`127.0.0.1`) is not supported by TRDP. Use one connection per local IP. |

Telegrams (`inputs` / `outputs`):

| Key | Default | Description |
|---|---|---|
| `comid` | | Communication ID, must match the SUT's dataset |
| `size` | | Payload size in bytes (up to 1432), must match the SUT's dataset |
| `dest_ip` | | Destination address of the telegram: multicast group or unicast IP. Required for inputs. For outputs, the multicast group to join (omit for unicast). |
| `period_ms` | `100` | Inputs: publish interval. Outputs: expected interval; after 3 missed periods a timeout is logged and the last values are kept. |
| `name` | | Optional, used in logs |
| `variables` | | `name`, `offset` (bytes from the start of the payload), `type` |

Values are big-endian. A `bool` occupies one byte (TRDP `BOOL8`). For single bits, use `"type": "bitset8"` (or `bitset16`, `bitset32`) with a `bits` list. Each named bit becomes a `bool` variable called `<name>.<bit>` (for example `door_alarm.obstacle`). Bit 0 is the most significant bit of the first byte. Use `""` or `null` to skip a reserved bit.

## How it behaves

- **One thread per connection.** Each connection connects, then repeatedly sends inputs and receives outputs. Protocol libraries never share a thread.
- **Automatic reconnection.** If a connection can't be established or is lost, ammio retries every second. Clients keep working throughout: reads return the last known value and its timestamp.
- **Quiet logs.** Problems are logged once when they appear and once when they clear (`connected`, `connection lost`, `'x': Illegal data address`, `'x': OK again`), never once per cycle. Use `-v` for details.
- **Problems with one variable don't drop the link.** Examples are a Modbus exception, an OPC UA node that doesn't exist, or a type mismatch. The variable is reported and everything else keeps flowing.

## Architecture

```
  test client (e.g. ammtest)
         │  JSON over nng REQ/REP
  ┌──────▼──────────────────────────────────────────────┐
  │  server.c ──► vars.c (variable store) ◄── conn.c    │ ammio
  │                                            │        │
  │                   protocols/modbus.c · opcua.c · trdp.c
  └────────────────────────────────────────────┼────────┘
                                               ▼
                                      System Under Test
```

| File | Role |
|---|---|
| `src/main.c` | Command line, startup, shutdown |
| `src/server.c` | Client API |
| `src/vars.c` | Variable store, types, value conversion |
| `src/conn.c` | Runs each connection: thread, cycle timing, reconnection, logging |
| `src/json.c` | Strict config reading with precise error messages |
| `src/protocols/*.c` | One file per protocol: parse config, connect, exchange data |

### Adding a protocol

A protocol is a `protocol_t` (see [`src/protocol.h`](src/protocol.h)) with five functions: `create` (validate config, register variables), `connect`, `cycle` (one exchange), `disconnect` and `destroy`. Threads, timing, reconnection and logging are handled by ammio. Add the file to `CMakeLists.txt` and the protocol to the table in `src/conn.c`.

## Building from source

Dependencies are git submodules and are built from source, so no system libraries are needed.

```bash
git clone --recursive https://github.com/ammonit-software/ammio.git
```

Linux (builds and runs the tests):

```bash
scripts/build_linux.sh
```

Windows (Visual Studio 2022 build tools):

```cmd
scripts\build_windows-x64.bat --config Release
```

The test suite (`-DAMMIO_TESTS=ON`, POSIX only) runs ammio against real in-process Modbus TCP and OPC UA servers and a TRDP loop, and also covers the client API, value conversion and config validation.

## Related Projects

- **[ammtest](https://github.com/ammonit-software/ammtest)**: Python test framework for critical-software systems. Write, run, and trace system-level tests against any System Under Test (SUT).
- **[Ammonit Software](https://github.com/ammonit-software)**: Parent organization.

## License

This project is open source and available under the [MIT License](LICENSE).

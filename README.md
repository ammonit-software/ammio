<p align="center">
  <img src="assets/ammio.png" alt="ammio" width="350"/>
</p>

<p align="center">
  <strong>Protocol agnostic test interface for critical-software systems. Talk to any System Under Test (SUT) in JSON, regardless of its protocol.</strong>
</p>

# ammio

**ammio** sits between your test tooling and your SUT. It speaks the SUT's protocol (Modbus, OPC UA, TRDP) on one side and exposes every variable through one small JSON API on the other.

- **No protocol barrier**: read and write SUT variables by id, without knowing the protocol.
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
        "inputs":  [ { "var_id": "motor_speed_actual",   "address": 0,  "type": "uint16" } ],
        "outputs": [ { "var_id": "motor_speed_setpoint", "address": 10, "type": "uint16" } ]
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

    ammio(cmd="write", var_id="motor_speed_actual", value=1200)
    print(ammio(cmd="read", var_id="motor_speed_setpoint"))
```

**[ammtest](https://github.com/ammonit-software/ammtest)** is the recommended client. It adds test scripting, pytest integration and reporting on top of ammio.

## Command line

```
ammio [options] <config.json>

  -i, --interface <file>  the config file (same as the positional argument)
  -e, --endpoint <url>    where clients connect (default: tcp://127.0.0.1:5555)
  -c, --check             validate the config and exit
  -v, --verbose           log debug details (same as --log-level 0)
      --log-level <n>     0 = debug, 1 = info (default), 2 = errors only
  -h, --help              show this help
      --version           show the version
```

The v0.5 form, `ammio --endpoint <url> --interface <file> --log-level <n>`, works unchanged. ammio stops cleanly on Ctrl+C or `SIGTERM`.

## Client API

Clients send JSON requests over nng REQ/REP (TCP by default). Every request gets exactly one JSON reply.

| Request | Reply |
|---|---|
| `{"cmd": "read", "var_id": "x"}` | `{"var_id": "x", "type": 2, "dir": "output", "value": 42, "timestamp": 1732460000123456789}` |
| `{"cmd": "write", "var_id": "x", "value": 42}` | `{"status": "ok"}` |
| `{"cmd": "list_vars"}` | `{"vars": [{"var_id": "x", "type": "uint16", "dir": "output"}, ...]}` |
| `{"cmd": "list_errors"}` | `{"errors": {"0": "ok", "1": "variable not found", ...}}` |

- `value` is a JSON number, or `true`/`false` for `bool` variables (numbers 0/1 are accepted too).
- In `read`, `type` is a numeric code: `0` uint8, `1` int8, `2` uint16, `3` int16, `4` uint32, `5` int32, `6` float32, `7` float64, `8` bool. `list_vars` gives the names.
- `timestamp` is the time of the last update, in nanoseconds since the Unix epoch: the last client write for inputs, the last value received from the SUT for outputs. `0` means never updated. Use it to detect stale data.
- Writes are exact. A value that doesn't fit the variable's type (out of range, or a fraction for an integer type) is rejected, never truncated.

Errors are returned as `{"error": <code>, "message": "<what went wrong>"}`:

| Code | Meaning |
|---|---|
| 1 | Unknown variable |
| 2 | Variable is read-only (an output) |
| 3 | Request is not a JSON object |
| 4 | Unknown command, or missing `cmd` / `var_id` |
| 5 | `write` without `value` |
| 6 | Value doesn't fit the variable's type |

## Configuration

A config file is a JSON object of **connections**. Each connection has a user-chosen name, a `protocol`, the protocol's connection settings, and lists of `inputs` and `outputs`. A config can hold any number of connections of any protocols.

Every variable has a `var_id`, the id clients use. Ids must be unique across the whole file. An optional `name` can hold a description; ammio ignores it.

| Direction | Data flow | Typical variables |
|---|---|---|
| `inputs` | client → ammio → SUT | Sensor readings, external device states, feedback the SUT consumes |
| `outputs` | SUT → ammio → client | Commands, setpoints, enable signals the SUT produces (read-only for clients) |

**Types**: `bool`, `uint8`, `int8`, `uint16`, `int16`, `uint32`, `int32`, `float32`, `float64`.

ammio is strict: unknown keys, missing keys, bad values, duplicate ids and overlapping addresses are all errors. A typo such as `"adress"` stops startup instead of silently becoming address 0.

Ready-to-use examples are in [`config/`](config).

### Modbus

ammio is the Modbus master (client). It writes inputs to holding registers and reads outputs from holding registers, once per `period_ms`.

```json
{
    "plc": {
        "protocol": "modbus",
        "host": "192.168.1.10",
        "inputs":  [ { "var_id": "motor_current", "address": 2,  "type": "float32" } ],
        "outputs": [ { "var_id": "motor_enable",  "address": 14, "type": "bool" } ]
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

Variables: `var_id`, `address` (0-based register), `type`. 8- and 16-bit types use one register, 32-bit types use two, `float64` uses four.

### OPC UA

ammio is an OPC UA client. Each period it writes all inputs in one request and reads all outputs in one request.

```json
{
    "scada": {
        "protocol": "opcua",
        "endpoint": "opc.tcp://192.168.1.20:4840",
        "inputs":  [ { "var_id": "motor_speed_actual", "node_id": "ns=1;s=Motor.SpeedActual", "type": "float32" } ],
        "outputs": [ { "var_id": "motor_enable",       "node_id": "ns=1;i=1001",              "type": "bool" } ]
    }
}
```

| Key | Default | Description |
|---|---|---|
| `endpoint` | | Server URL |
| `username`, `password` | | Optional, set both or neither |
| `period_ms` | `100` | Read/write cycle |

Variables: `var_id`, `node_id` (`ns=1;s=Name`, `ns=1;i=1001`, ... use a browser such as UaExpert to find them), `type`. The type must match the node's data type (`bool` = Boolean, `uint8` = Byte, `float32` = Float, `float64` = Double, ...). A mismatch is reported and the value is never misread.

### TRDP

ammio exchanges TRDP process data (`"type": "Pd"`, the default) and message data (`"Mn"`, `"Mr"`, `"Mp"`).

| Key | Default | Description |
|---|---|---|
| `local_ip` | `"0.0.0.0"` | Local interface to use. Loopback (`127.0.0.1`) is not supported by TRDP. Use one connection per local IP. |
| `etb_topo_cnt_id`, `op_trn_topo_cnt_id` | | Optional: create a `uint32` input with this `var_id`, holding the ETB / operational train topology counter of the session |

Every telegram in `inputs` / `outputs` can have an **`enable_id`**: ammio creates a `uint8` input with that `var_id`. What it does depends on the telegram (see below).

#### Process data (`Pd`)

```json
{
    "var_id": "DOOR_STATUS", "enable_id": "DOOR_STATUS.is_enabled",
    "comid": 2000, "dest_ip": "239.255.1.2", "period_ms": 100, "size": 4,
    "variables": [
        { "var_id": "door_is_open", "offset": 0, "type": "uint8" },
        { "var_id": "door_alarm",   "offset": 3, "type": "bitset8", "bits": ["obstacle", "emergency_release"] }
    ]
}
```

| Key | Default | Description |
|---|---|---|
| `comid` | | Communication ID, must match the SUT's dataset |
| `size` | | Payload size in bytes (up to 1432), must match the SUT's dataset |
| `dest_ip` | | Destination of the telegram: multicast group or unicast IP. Required for inputs. For outputs, the multicast group to join (omit for unicast). |
| `period_ms` | `100` | Inputs: publish interval. Outputs: expected interval; after 3 missed periods a timeout is logged and the last values are kept. |
| `enable_id` | | Starts as 1. Inputs: 0 stops publishing, 1 starts again. Outputs: 0 stops updating the variables. |
| `var_id`, `name` | | Optional, used in logs |
| `variables` | | `var_id`, `offset` (bytes from the start of the payload), `type` |

Values are big-endian. A `bool` occupies one byte (TRDP `BOOL8`). For single bits, use `"type": "bitset8"` (or `bitset16`, `bitset32`) with a `bits` list:
- each bit becomes a `bool` variable `<var_id>.<bit name>` (for example `door_alarm.obstacle`), or `<var_id>.bit<N>` when its name is `""` or `null`;
- the whole bitset is also available as `<var_id>` (`uint8`/`uint16`/`uint32`). In inputs it is read-only and shows the assembled bits.

Bit 0 is the most significant bit of the first byte.

#### Message data (`Mn`, `Mr`, `Mp`)

```json
{
    "type": "Mr", "var_id": "status_request", "enable_id": "status_request.send",
    "comid": 3000, "dest_ip": "10.0.0.5", "timeout_ms": 3000,
    "variables": [ { "var_id": "device_id", "type": "uint16" } ]
}
```

A message's `var_id` is required: its values are available as `<message var_id>.<var_id>`. The fields are laid out one after another, with no offsets. Each variable is one of:
- **a value**: `{"var_id": "speed", "type": "uint16"}`;
- **padding**: `{"type": "padding", "bits": 16}` (a multiple of 8), sent as zeros;
- **an array of structs**: `{"var_id": "cars", "type": "car", "iterations_id": "car_count", "iterations_type": "uint8", "max_iterations": 8}`. Here `car` is defined in the message's `structs`, e.g. `"structs": {"car": [{"var_id": "id", "type": "uint16"}, {"type": "padding", "bits": 16}]}`. On the wire, the number of entries (as `iterations_type`) comes first, then the entries. Each entry member becomes `<message>.<var_id>[<i>].<member>` (for example `train_info.cars[0].id`) and the count becomes `<message>.<iterations_id>`.

| Telegram | Behaviour |
|---|---|
| `Mn` in inputs | Sent to `dest_ip` each time the client writes `1` to its `enable_id` (required). ammio then resets the flag to `0`. |
| `Mr` in inputs | Sent the same way, as a request. The reply is stored in the `Mp` in outputs whose `reply_to` is this `Mr`'s `var_id` (required). Optional `timeout_ms` (default 3000) bounds the wait; a missing reply is logged. |
| `Mp` in inputs | `reply_to` names an `Mr` in outputs. Each time the SUT sends that request, ammio stores its values and answers with this `Mp`. |
| `Mn` / `Mr` in outputs | Received from the SUT; the values are stored. |
| `Mp` in outputs | Receives the replies to the `Mr` in inputs named by its `reply_to`. |

Except for `Mn`/`Mr` inputs, `enable_id` is optional, starts as 1, and 0 makes ammio ignore the telegram.

## How it behaves

- **One thread per connection.** Each connection connects, then repeatedly sends inputs and receives outputs. Protocol libraries never share a thread.
- **Automatic reconnection.** If a connection can't be established or is lost, ammio retries every second. Clients keep working throughout: reads return the last known value and its timestamp.
- **Quiet logs.** Problems are logged once when they appear and once when they clear (`connected`, `connection lost`, `'x': Illegal data address`, `'x': OK again`), never once per cycle. Use `-v` for details.
- **Problems with one variable don't drop the link.** Examples are a Modbus exception, an OPC UA node that doesn't exist, or a type mismatch. The variable is reported and everything else keeps flowing.

## Migrating from v0.5

The client API and the command line are unchanged: `var_id`, `read`/`write`/`list_vars`/`list_errors`, the same error codes, and `--endpoint`, `--interface`, `--log-level`. Replies may contain extra fields (`dir`, `message`).

Config files have a flatter format:

| v0.5 | Now |
|---|---|
| `{"plc": {"interface": "modbus", "specification": {...}}}` | `{"plc": {"protocol": "modbus", ...}}` |
| `"registers"` / `"nodes"` / `"containers"`: `{"inputs": [...], "outputs": [...]}` | `"inputs": [...]`, `"outputs": [...]` directly in the connection |
| Modbus `"mode": "tcp"` / `"rtu"` | Set `host` (TCP) or `device` (RTU) |
| TRDP `size_bits`, bit `offset` | `size` and `offset` in bytes |
| TRDP `multicast_ip` | `dest_ip` |

Everything else (`var_id`, `enable_id`, message data, `structs`, `reply_to`, topology counters, bitsets) keeps its meaning. Keys that v0.5 silently defaulted when missing (such as `type`) are now required. ammio recognises a v0.5 file and points here.

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

The test suite (`-DAMMIO_TESTS=ON`, POSIX only) runs ammio against real in-process Modbus TCP and OPC UA servers and a TRDP loop (process and message data), and also covers the client API, value conversion and config validation.

## Related Projects

- **[ammtest](https://github.com/ammonit-software/ammtest)**: Python test framework for critical-software systems. Write, run, and trace system-level tests against any System Under Test (SUT).
- **[Ammonit Software](https://github.com/ammonit-software)**: Parent organization.

## License

This project is open source and available under the [MIT License](LICENSE).

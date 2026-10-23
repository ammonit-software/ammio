<p align="center">
  <img src="assets/ammio.png" alt="ammio" width="350"/>
</p>

<p align="center">
  <strong>One JSON API for the signals of a System Under Test, whatever protocol it speaks: TRDP, Modbus or OPC UA.</strong>
</p>

# ammio

**ammio** sits between your test tooling and a System Under Test (SUT). On one side it speaks the
SUT's protocol; on the other it exposes every configured signal as a named variable over a small
JSON API. Test scripts read and write `door_open_cmd` instead of dealing with TRDP telegrams,
Modbus register maps or OPC UA node ids.

What this gives you:

- **No protocol barrier.** Scripts in any language can read and write SUT signals without a
  protocol stack or specialist knowledge.
- **Stable test scripts.** Variable names come from the configuration file. When the SUT moves
  from Modbus to OPC UA, or a TRDP dataset changes, you update the configuration, not every script.
- **Visible data quality.** Every value carries a quality (`good`, `stale`, `disconnected`, ...)
  and a timestamp, so a test can tell "the SUT says 0" apart from "the SUT stopped talking".

What ammio is **not**:

- **Not a hardware-in-the-loop bench.** It replaces the protocol adapter, not the SUT, the I/O or
  the timing environment. You still need the SUT running (on hardware or as a software build) and
  reachable on the network.
- **Not a qualified tool.** If you use ammio to produce verification evidence under a safety
  standard (EN 50128, IEC 61508, ...), tool classification and qualification are your
  responsibility. The automated tests in this repository are a starting point for that evidence,
  not a substitute for it.
- **Not hard real-time.** Cycles are scheduled without drift, but on a general-purpose OS with no
  latency guarantees.

## Quickstart

### 1. Download

Get `ammio-windows-x64.zip` or `ammio-linux-x64.tar.gz` from the
[latest release](https://github.com/ammonit-software/ammio/releases/latest). Each archive contains
the `ammio` binary and the example configurations from [`config/`](config).

### 2. Configure

Start from the example for your protocol (`modbus.example.json`, `opcua.example.json` or
`trdp.example.json`), adapt addresses and variables, and check it:

```sh
ammio --check --config my_sut.json
```

`--check` reports every problem with its location, e.g.
`config: plc_1.settings.registers.inputs[2]: unknown key 'adress'`, and exits non-zero.

### 3. Run

```sh
ammio --config my_sut.json --endpoint tcp://127.0.0.1:5555
```

### 4. Interact

**[ammtest](https://github.com/ammonit-software/ammtest)** is the recommended client: it adds test
scripting, pytest integration and reporting on top of ammio. Any language with an
[nng](https://nng.nanomsg.org/) binding works too. In Python with `pynng`:

```python
import json, pynng

with pynng.Req0(dial="tcp://127.0.0.1:5555") as sock:
    def call(**req):
        sock.send(json.dumps(req).encode())
        return json.loads(sock.recv())

    call(cmd="write", var_id="motor_speed_actual", value=1200)
    print(call(cmd="read", var_id="motor_speed_setpoint"))
    # {"ok": true, "var_id": "motor_speed_setpoint", "value": 1150, "type": "uint16",
    #  "dir": "output", "quality": "good", "timestamp_ns": 1761234567890123456}
```

## Concepts

### Connections and variables

The configuration file is a JSON object. Each top-level key is a **connection**: a named link to
the SUT using one protocol. A file can hold several connections, of different protocols or of the
same one (two PLCs, two TRDP interfaces, ...). Each connection runs in its own thread.

A connection declares **variables**. Each has a `var_id`, which is the name clients use. `var_id`s
are exactly what you write in the configuration and must be unique across the whole file. ammio
generates ids only where the configuration cannot name every variable:

| Generated id | Where |
|---|---|
| `<connection>.connected` | every connection: `true` while the link to the SUT is up (read-only) |
| `<container>.is_enabled` | TRDP containers, unless `enable_id` is given |
| `<bitset>.<bit>` | TRDP bitset members (`<bitset>.bit<N>` for unnamed bits) |
| `<array>[i].<field>` | entries of TRDP message-data arrays |
| `<dataset>.<element>` | telegrams imported from a TRDP XML file |

### Direction

Directions are named from the **SUT's** point of view:

| Direction | Data flow | Client access | Typical signals |
|---|---|---|---|
| `input` | ammio → SUT | read / write | sensor values, feedback and states the SUT consumes |
| `output` | SUT → ammio | read only | commands, setpoints and states the SUT produces |

### When values are sent to the SUT

ammio never makes up data for the SUT:

- An input is sent only once it **has a value**: written by a client, or given as `"initial"` in
  the configuration. Until then the SUT's own value is left alone. Starting ammio does not write
  zeros to a PLC.
- Modbus and OPC UA inputs are written **when a client writes them** (`"write_mode": "on_change"`,
  the default). Writing the same value again sends it again. With `"write_mode": "cyclic"` every
  input with a value is written every period. After a reconnect, every input with a value is
  sent once more.
- A TRDP process-data telegram is published once any of its variables has a value, and only while
  its `<container>.is_enabled` variable is `true`. Setting it to `false` really stops the telegram,
  so the SUT sees a timeout.

### Quality and timestamps

Every variable carries a `quality`:

| Quality | Meaning |
|---|---|
| `no_data` | no value yet (never received, never written) |
| `good` | current value |
| `stale` | last known value, but the SUT stopped sending it (TRDP timeout, missing MD reply, OPC UA "uncertain") |
| `disconnected` | the connection to the SUT is down. Old values are kept but flagged. |
| `bad` | the SUT rejected the request or sent a value that does not fit the variable's type |

`timestamp_ns` is the wall-clock time (nanoseconds since the UNIX epoch) of the last update. It is
sent as an exact JSON integer.

### Types

`bool`, `uint8`, `int8`, `uint16`, `int16`, `uint32`, `int32`, `uint64`, `int64`, `float32`,
`float64`. Writes are range-checked: `300` for a `uint8` or `1.5` for a `uint16` is rejected, never
truncated. JSON `true`/`false` are accepted for any integer type. 64-bit integers above 2^53 can be
read exactly but not written, because JSON numbers are doubles.

## JSON API

Clients use an nng **REQ** socket connected to `--endpoint`. Each request is one JSON object with a
`cmd`. If the request has an `"id"`, the response echoes it. Every response contains `"ok"`.

| Command | Request | Response |
|---|---|---|
| `read` | `{"cmd": "read", "var_id": "x"}` | `{"ok": true, "var_id": "x", "value": 1, "type": "uint8", "dir": "input", "quality": "good", "timestamp_ns": ...}` |
| `read` (several) | `{"cmd": "read", "var_ids": ["x", "y"]}` | `{"ok": true, "vars": [{...}, {...}]}`: one consistent snapshot |
| `write` | `{"cmd": "write", "var_id": "x", "value": 1}` | `{"ok": true}` |
| `write` (several) | `{"cmd": "write", "values": {"x": 1, "y": 2.5}}` | `{"ok": true}`: all values or none, applied atomically |
| `list_vars` | `{"cmd": "list_vars"}` | `{"ok": true, "vars": [{"var_id", "type", "dir", "access": "rw" \| "r", "connection"}]}` |
| `list_connections` | `{"cmd": "list_connections"}` | `{"ok": true, "connections": [{"name", "protocol", "state"}]}` |
| `info` | `{"cmd": "info"}` | `{"ok": true, "name": "ammio", "version", "protocols", "events"}` |

Use batch writes for values that belong together, for example several fields of one TRDP
telegram. They are applied under one lock and the driver snapshots a telegram's variables
together, so the SUT never sees half of an update.

Errors look like `{"ok": false, "error": {"code": "READ_ONLY", "message": "...", "var_id": "x"}}`:

| Code | Meaning |
|---|---|
| `INVALID_JSON` | the request is not valid JSON |
| `INVALID_REQUEST` | not an object, missing or malformed fields |
| `UNKNOWN_CMD` | unknown `cmd` |
| `NOT_FOUND` | unknown `var_id` |
| `READ_ONLY` | the variable is an output of the SUT |
| `INVALID_VALUE` | wrong JSON type, or a fraction for an integer variable |
| `OUT_OF_RANGE` | the value does not fit the variable's type |
| `INTERNAL` | out of memory |

### Change events

With `--events <url>` ammio also listens on an nng **PUB** socket and publishes one message per
change of a value or quality:

```json
{"var_id":"door_open_cmd","value":1,"quality":"good","timestamp_ns":1761234567890123456}
```

Messages start with `{"var_id":"<id>"`, so a SUB socket can subscribe to one variable (or a prefix)
by subscribing to that string. Events let a test catch a 20 ms pulse that polling would miss.

## Configuration

### Command line

| Argument | Description |
|---|---|
| `--config <file>` | connections file (required) |
| `--endpoint <url>` | nng endpoint of the JSON API, e.g. `tcp://127.0.0.1:5555` (required unless `--check`) |
| `--events <url>` | optional nng PUB endpoint for change events |
| `--log-level <level>` | `debug`, `info` (default), `warn`, `error` or `off` |
| `--check` | validate the configuration and exit (0 = valid) |
| `--version`, `--help` | |

### File format

```jsonc
{
    "<connection name>": {
        "protocol": "modbus" | "opcua" | "trdp",
        "settings": { ... }            // protocol specific, see below
    }
}
```

Validation is strict. Unknown keys, missing required keys, out-of-range numbers, duplicate
`var_id`s and inconsistent layouts are errors, and all of them are reported in one run. JSON has no
comments: use `"description"` or any key starting with `_` to annotate an object. Complete
examples are in [`config/`](config).

### Modbus (client / master)

| Setting | Default | Description |
|---|---|---|
| `mode` | `"tcp"` | `"tcp"` or `"rtu"` |
| `host`, `port` | –, `502` | TCP: slave address (required) and port |
| `device`, `baud`, `parity`, `data_bits`, `stop_bits` | –, `9600`, `"N"`, `8`, `1` | RTU: serial device (required) and line settings |
| `slave_id` | `1` | unit id, 0–255 |
| `period_ms` | `100` | poll and write cycle |
| `timeout_ms` | `500` | response timeout |
| `byte_order` | `"ABCD"` | byte order of multi-register values: `ABCD` (big-endian), `CDAB` (word swap), `BADC` (byte swap), `DCBA` (little-endian) |
| `write_mode` | `"on_change"` | `"on_change"` or `"cyclic"` (see above) |
| `registers.inputs[]`, `registers.outputs[]` | | variables |

Each variable has `var_id`, `address` (0-based), `type` and optionally `area`, `byte_order`
(overrides the connection's) and, for inputs, `initial`:

| `area` | Modbus table | Direction | Types |
|---|---|---|---|
| `holding` (default) | holding registers (FC 3 / 6 / 16) | input or output | all (8-bit types use the low byte; 32-bit: 2 registers, 64-bit: 4) |
| `coil` | coils (FC 1 / 5 / 15) | input or output | `bool` |
| `input_register` | input registers (FC 4) | output | all |
| `discrete_input` | discrete inputs (FC 2) | output | `bool` |

Outputs are read in batches of contiguous addresses. If the slave rejects a request (Modbus
exception), only the variables it covered become `bad`. Timeouts and broken connections mark the
whole connection `disconnected`, and ammio reconnects.

### OPC UA (client)

| Setting | Default | Description |
|---|---|---|
| `endpoint` | | `opc.tcp://host:port[/path]` (required) |
| `username`, `password` | | optional, both or neither |
| `period_ms` | `100` | sampling / publishing interval (subscription) or poll interval |
| `timeout_ms` | `5000` | request timeout |
| `read_mode` | `"subscription"` | `"subscription"` (monitored items) or `"poll"` (one batched Read per period) |
| `write_mode` | `"on_change"` | `"on_change"` or `"cyclic"` |
| `nodes.inputs[]`, `nodes.outputs[]` | | variables: `var_id`, `node_id`, `type`, and for inputs `initial` |

`node_id` uses the standard string form: `ns=1;s=Motor.Speed`, `ns=2;i=1001`, `i=85`, `ns=1;g=<guid>`,
`ns=1;b=<base64>`. Values are converted from whatever numeric type the server uses, with range
checks. A value that does not fit becomes `bad` instead of being truncated. Writes use the OPC UA
type matching the variable's `type`, which must match the node's data type. The OPC UA status code
of each value becomes its quality. The connection uses security mode None.

### TRDP

| Setting | Default | Description |
|---|---|---|
| `local_ip` | `"0.0.0.0"` | address of the NIC to use. TRDP multicast does not work on loopback. |
| `etb_topo_cnt_id`, `op_trn_topo_cnt_id` | | optional `var_id`s of `uint32` inputs that set the ETB / operational train topology counters |
| `containers.inputs[]`, `containers.outputs[]` | | telegrams (below) |
| `xml` | | import telegrams from a TRDP XML device configuration (below) |

#### Process data (`"type": "Pd"`, the default)

| Key | Inputs | Outputs | Description |
|---|---|---|---|
| `id` | required | required | container identifier (used in logs and `<id>.is_enabled`) |
| `name` | optional | optional | display label |
| `comid` | required | required | |
| `dest_ip` | required | – | unicast or multicast destination |
| `multicast_ip` | – | optional | multicast group to join |
| `source_ip` | – | optional | only accept telegrams from this source |
| `period_ms` | `100` | `100` | publish cycle; for outputs only used for the default timeout |
| `timeout_ms` | – | `3 × period_ms` | after this the variables become `stale` (last value kept) |
| `size_bits` | required | required | payload size, a multiple of 8 |
| `enable_id`, `enabled` | `<id>.is_enabled`, `true` | same | the enable variable and its initial value. Inputs: `false` stops publishing. Outputs: `false` freezes the received values. |
| `variables[]` | required | required | `var_id`, `offset` (bits, byte-aligned), `type`, and for inputs `initial` |

Bitsets: `"type": "bitset8" | "bitset16" | "bitset32"` with `"bits": ["name0", "name1", ...]` gives
one variable for the whole word plus one `bool` per named bit. Bit 0 is the most significant bit of
the first byte. Clients can write either the word or single bits. Whichever was written last wins,
and the other side follows.

Offsets must be byte-aligned and every variable must fit within `size_bits` without overlapping
another one. Data is encoded big-endian and packed (TRDP wire format).

#### Message data (`"type": "Mn" | "Mr" | "Mp"`)

| Type | In `inputs` (ammio sends) | In `outputs` (ammio receives) |
|---|---|---|
| `Mn` | notification to `dest_ip`, sent each time a client sets `<id>.is_enabled` to `true` (it resets itself) | notifications received on `comid` (optionally `multicast_ip`) |
| `Mr` | request to `dest_ip`. The reply arrives in the `outputs` `Mp` whose `reply_to` names it. `reply_timeout_ms` (default 3000) marks that reply `stale` when it does not come. | requests from the SUT: their payload is stored, and ammio answers with the `inputs` `Mp` whose `reply_to` names it |
| `Mp` | ammio's reply data for an `outputs` `Mr`. `enabled: false` stops answering, to test SUT timeouts. | reply data for an `inputs` `Mr` |

Message variables are listed in wire order: scalars (`var_id`, `type`), padding
(`{"type": "padding", "bits": 16}`), or arrays of structs declared in `structs`:

```jsonc
"structs": { "Entry": [ {"var_id": "id", "type": "uint8"}, {"var_id": "level", "type": "int16"} ] },
"variables": [
    {"var_id": "entries", "type": "Entry", "iterations_id": "entry_count",
     "iterations_type": "uint8", "max_iterations": 8}      // -> entry_count, entries[0].id, ...
]
```

The element count (`iterations_id`) precedes the entries on the wire.

#### Importing a TRDP XML device configuration

```json
"settings": {
    "local_ip": "10.0.0.1",
    "xml": { "file": "door_controller.xml", "interface": "eth0", "perspective": "sut" }
}
```

Process-data telegrams of the bus interface are imported with their datasets, including nested
datasets and arrays. Variables are named `<dataset name>.<element>`, e.g.
`DoorStatus.door[1].force`. TIMEDATE48/64 become `.seconds` plus `.ticks` / `.microseconds`. With
`"perspective": "sut"` (the default) the XML describes the SUT: its `source` telegrams become
outputs, its `sink` telegrams inputs. Use `"ammio"` if the XML describes the device ammio stands in
for. Imported and hand-written containers can be combined. Message-data telegrams and
variable-size arrays are not imported.

## Architecture

```
  test client (ammtest, scripts, ...)
         │  nng REQ/REP, JSON          ▲ nng PUB/SUB, change events (optional)
  ┌──────▼─────────────────────────────┴──────────────────────────────────┐
  │  api/          api_server: requests        events: change publisher   │
  │  ──────────────────────────────────────────────────────────────────── │
  │  core/         var_table: values, quality, timestamps (thread-safe)   │
  │                connections: one instance per configured connection    │
  │  ──────────────────────────────────────────────────────────────────── │
  │  drivers/      modbus        opcua         trdp (+ trdp_xml)          │
  │                one thread per connection, own protocol context        │
  └───────────────────┬────────────────┬────────────────┬─────────────────┘
                 Modbus TCP/RTU      OPC UA            TRDP PD / MD
                      ▼                ▼                ▼
                          System Under Test (SUT)
```

- **api/**: `api_server` parses each request using its exact length (nng messages are not
  NUL-terminated), checks direction, type and range, and answers. `events` publishes changes.
- **core/**: `var_table` holds every variable. Writes and reads of several variables happen under
  one lock. Variables are never removed while ammio runs, so drivers keep direct handles.
  `connections` builds one driver instance per configuration entry and tracks its link state.
- **drivers/**: each driver implements `create` (parse and validate, no network), `start`,
  `stop` and `destroy`. Protocol libraries that are not thread-safe are only used from their
  connection's thread.

## Building from source

```sh
git clone --recursive https://github.com/ammonit-software/ammio
cmake -S ammio -B ammio/build -DCMAKE_BUILD_TYPE=Release
cmake --build ammio/build
```

On Windows, `scripts\build_windows-x64.bat --config Release` builds with Visual Studio 2022.
All dependencies are vendored as submodules: cJSON, uthash, nng, TCNopen, libmodbus, open62541.

### Tests

```sh
ctest --test-dir build --output-on-failure                 # C unit tests
pip install -r tests/integration/requirements.txt
AMMIO_BIN=build/ammio pytest tests/integration             # against simulated devices
```

The integration tests run the real binary against a pymodbus slave, an asyncua server and a second
ammio instance over TRDP. The TRDP tests need a non-loopback IPv4 address (`AMMIO_TRDP_IP`
overrides it). CI runs everything on Linux, also under AddressSanitizer and
UndefinedBehaviorSanitizer, and on Windows.

## Upgrading from 0.5

| 0.5 | 0.6 |
|---|---|
| `--interface <file>` | `--config <file>` (old flag still accepted, with a warning) |
| `"interface": "modbus"`, `"specification": {...}` | `"protocol": "modbus"`, `"settings": {...}` (old keys still accepted, with a warning) |
| TRDP container `var_id` | `id` (old key still accepted, with a warning) |
| TRDP `enable_id` required | optional, defaults to `<id>.is_enabled`; enable variables are `bool` |
| TRDP PD inputs `multicast_ip` | `dest_ip` (`multicast_ip` still accepted for inputs) |
| TRDP MD variables named `<container>.<var_id>` | named exactly `<var_id>`. Array entries are `<array>[i].<field>`. |
| Modbus `float32` used word order CDAB, integers ABCD | everything uses `byte_order` (default `ABCD`). Set `"byte_order": "CDAB"` on `float32` variables to keep the old behaviour. |
| missing `type` / `address` defaulted to `uint16` / `0` | required |
| inputs written as `0` from startup | written only once they have a value |
| responses `{"error": 1}` / `{"status": "ok"}`, numeric `type` in `read` | `{"ok": ..., "error": {"code", "message"}}`, `type` as a string |
| `list_errors` | removed (error codes are self-describing strings) |
| values never received read as `0` | `null` with quality `no_data` |

## Related Projects

- **[ammtest](https://github.com/ammonit-software/ammtest)**: Python test framework for critical-software systems. Write, run, and trace system-level tests against any System Under Test (SUT).
- **[Ammonit Software](https://github.com/ammonit-software)**: Parent organization.

## License

This project is open source and available under the [MIT License](LICENSE).

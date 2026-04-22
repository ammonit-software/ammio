<p align="center">
  <img src="assets/ammio.png" alt="ammio" width="350"/>
</p>

<p align="center">
  <strong>Protocol agnostic test interface for critical-software systems. Talk to any System Under Test (SUT) in JSON, regardless of its protocol.</strong>
</p>

# ammio

**ammio** is a protocol agnostic test interface for critical-software systems. It sits between your test tooling and your SUT, speaking the SUT's native protocol on one side and exposing all its variables over a plain JSON API on the other. Talk to any software system in JSON, regardless of its protocol.

It addresses three core pain points in critical systems testing:

- *The protocol barrier*: SUTs speak TRDP, CAN, OPC UA, Modbus — each requiring specialist knowledge just to read a variable. **ammio** abstracts all of it into a single JSON API.
- *The feedback loop speed*: Hardware-in-the-loop benches take days to set up. **ammio** lets scripts and automated tests interact with the SUT immediately, without a physical bench.
- *The protocol migration problem*: When a SUT changes or adds a protocol, every tool and test script that talked to it breaks. With **ammio** in the middle, the JSON API stays the same — no refactoring, no disruption.

## Quickstart

### 1. Download

#### Windows
```cmd
mkdir ammio
cd ammio
curl -L -o ammio-windows-x64.zip https://github.com/ammonit-software/ammio/releases/latest/download/ammio-windows-x64.zip
tar -xf ammio-windows-x64.zip
curl -L -o interface.json https://raw.githubusercontent.com/ammonit-software/ammio/main/config/interface.example.json

```

Tinker the `interface.json` and adapt it for your needs. See [Configuration](#configuration) for more details.

### 2. Run

#### Windows
```cmd
ammio.exe --endpoint tcp://127.0.0.1:5555 --interface interface.json
```

### 3. Interact

#### Using ammtest (recommended)

**[ammtest](https://github.com/ammonit-software/ammtest)** is the recommended client — it adds test scripting, pytest integration, and result reporting on top of ammio.

#### Using a plain nng client

Any language with an nng binding works.

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

### Command-line arguments

| Argument | Required | Description |
|---|---|---|
| `--endpoint <url>` | yes | nng endpoint where ammio listens for client connections (e.g. `tcp://127.0.0.1:5555`) |
| `--interface <path>` | yes | path to the interface JSON file |
| `--log-level <N>` | no | log verbosity: `0` = debug, `1` = info (default: info) |

### `interface.json`

Each entry is a user-defined connection name bound to a protocol and its specification. Multiple connections of different types can coexist in the same file.

**Variable direction** defines the data flow between ammio and the SUT:

| Direction | Data flow | Typical variables |
|---|---|---|
| `input` | ammio → SUT | Sensor readings, external device states, status feedback that the SUT consumes |
| `output` | SUT → ammio | Commands, setpoints, enable signals that the SUT produces |

See `config/interface.x.example.json` files for ready-to-use starting points depending on the protocol.

#### Modbus

```jsonc
{
    "plc_connection": {                             // user-defined connection name
        "interface": "modbus",
        "specification": {
            "mode": "tcp",                          // "tcp" for Ethernet, "rtu" for serial
            "host": "192.168.1.10",                 // slave IP address (TCP only)
            "port": 502,                            // slave port, default 502 (TCP only)
            "slave_id": 1,                          // Modbus device address (1-247)
            "period_ms": 100,                       // read/write cycle interval in ms
            "registers": {
                "inputs": [                         // holding registers ammio writes (SUT inputs)
                    {
                        "var_id": "motor_speed_actual",   // variable identifier in var_table and JSON API
                        "address": 0,               // 0-based register address on the slave
                        "type": "uint16"            // uint8 int8 uint16 int16 uint32 int32 float32 float64
                    }
                ],
                "outputs": [                        // holding registers ammio reads (SUT outputs)
                    {
                        "var_id": "motor_speed_setpoint",
                        "address": 10,
                        "type": "uint16"
                    }
                ]
            }
        }
    }
}
```

#### TRDP

```jsonc
{
    "train_network": {                              // user-defined connection name
        "interface": "trdp",
        "specification": {
            "local_ip": "192.168.1.100",            // local NIC to bind to
                                                    // use "0.0.0.0" for the default interface
                                                    // loopback (127.0.0.1) is not supported
            "containers": {
                "inputs": [                         // containers ammio sends (SUT inputs)
                    {
                        "name": "DOOR_STATUS",      // container name, used for logging
                        "enable_id": "DOOR_STATUS.is_enabled", // var_table key for the enable flag — must be unique
                        "type": "Pd",               // Pd: process data (periodic). Mn: MD notify. Mr: MD request. Mp: MD reply.
                        "comid": 2000,              // TRDP communication ID, must match SUT dataset
                        "multicast_ip": "239.255.1.2", // multicast group IP (Pd only)
                        "period_ms": 100,           // publish interval in ms (Pd only)
                        "size_bits": 32,            // total payload size in bits (Pd only)
                        "variables": [              // variable mappings packed inside this container
                            {
                                "name": "door_is_open", // human-readable variable name
                                "var_id": "door_is_open", // unique key in var_table and JSON API
                                "offset": 0,        // bit offset within the container payload
                                "type": "uint8"     // uint8 int8 uint16 int16 uint32 int32 float32 float64
                            }
                        ]
                    },
                    {
                        "name": "ttdb_notification", // MD Mn example — one-shot notify sent to SUT
                        "enable_id": "ttdb_notification.is_enabled", // write 1 to fire; auto-resets to 0 after send
                        "type": "Mn",
                        "comid": 101,
                        "dest_ip": "239.255.0.1",   // unicast or multicast destination
                        "variables": [
                            { "name": "etbId", "var_id": "etbId", "offset": 0, "type": "uint8" }
                        ]
                    }
                ],
                "outputs": [                        // containers ammio receives (SUT outputs)
                    {
                        "name": "DOOR_CONTROL",
                        "enable_id": "DOOR_CONTROL.is_enabled",
                        "type": "Pd",
                        "comid": 1000,
                        "multicast_ip": "239.255.1.1",
                        "period_ms": 100,
                        "size_bits": 32,
                        "variables": [
                            { "name": "door_open_cmd", "var_id": "door_open_cmd", "offset": 0, "type": "uint8" }
                        ]
                    }
                ]
            }
        }
    }
}
```

For TRDP MD containers in `inputs`:

- `Mn`: raising `enable_id` sends one notification.
- `Mr`: raising `enable_id` sends one request and waits for the paired `Mp`.
- `Mp`: if the paired `Mr` arrives from the SUT, ammio replies automatically via `reply_to`.
- Proactive `Mn` and `Mr` sends require `dest_ip` to point to the SUT or multicast destination. If `dest_ip` is missing, ammio skips the send.

#### OPC UA

```jsonc
{
    "scada_server": {                               // user-defined connection name
        "interface": "opcua",
        "specification": {
            "endpoint": "opc.tcp://192.168.1.20:4840", // OPC UA server URL
            "username": "operator",                 // optional, for authenticated servers
            "password": "password",                 // optional
            "period_ms": 100,                       // read/write cycle interval in ms
            "nodes": {
                "inputs": [                         // nodes ammio writes to (SUT inputs)
                    {
                        "var_id": "motor_speed_actual",   // variable identifier in var_table and JSON API
                        "node_id": "ns=1;s=Motor.SpeedActual", // OPC UA node identifier
                                                    // ns=<namespace>;s=<string name>
                                                    // ns=<namespace>;i=<numeric id>
                                                    // browse node IDs with UAExpert or similar tool
                        "type": "float32"           // uint8 int8 uint16 int16 uint32 int32 float32 float64
                    }
                ],
                "outputs": [                        // nodes ammio reads from (SUT outputs)
                    {
                        "var_id": "motor_speed_setpoint",
                        "node_id": "ns=1;s=Motor.SpeedSetpoint",
                        "type": "float32"
                    }
                ]
            }
        }
    }
}
```

## Related Projects

- **[ammtest](https://github.com/ammonit-software/ammtest)**: Python test framework for critical-software systems. Write, run, and trace system-level tests against any System Under Test (SUT).
- **[Ammonit Software](https://github.com/ammonit-software)**: Parent organization.

## License

This project is open source and available under the [MIT License](LICENSE).

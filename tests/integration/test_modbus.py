import struct
import time

import pytest

from conftest import free_port, wait_until
from modbus_sim import FC_COILS, FC_DISCRETE, FC_HOLDING, FC_INPUT_REGS, ModbusSim


@pytest.fixture
def sim():
    s = ModbusSim(free_port())
    s.start()
    yield s
    s.stop()


def modbus_config(port, **settings):
    base = {
        "mode": "tcp",
        "host": "127.0.0.1",
        "port": port,
        "period_ms": 20,
        "timeout_ms": 200,
        "registers": {
            "inputs": [
                {"var_id": "speed_cmd", "address": 0, "type": "uint16"},
                {"var_id": "temp_cmd", "address": 1, "type": "float32"},
                {"var_id": "counter_cmd", "address": 3, "type": "int32"},
                {"var_id": "stop_cmd", "address": 5, "type": "bool", "area": "coil"},
                {"var_id": "with_initial", "address": 20, "type": "uint16", "initial": 77},
            ],
            "outputs": [
                {"var_id": "speed", "address": 100, "type": "uint16"},
                {"var_id": "temp", "address": 101, "type": "float32"},
                {"var_id": "temp_swapped", "address": 103, "type": "float32", "byte_order": "CDAB"},
                {"var_id": "alarm", "address": 7, "type": "bool", "area": "discrete_input"},
                {"var_id": "uptime", "address": 10, "type": "uint32", "area": "input_register"},
            ],
        },
    }
    base.update(settings)
    return {"plc": {"protocol": "modbus", "settings": base}}


def regs_float(value: float) -> list[int]:
    hi, lo = struct.unpack(">HH", struct.pack(">f", value))
    return [hi, lo]


def test_nothing_is_written_until_a_value_exists(sim, start_ammio):
    sim.set(FC_HOLDING, 0, [1234])
    a = start_ammio(modbus_config(sim.port))
    wait_until(lambda: a.client.value("plc.connected") is True, message="connection")
    time.sleep(0.2)
    # speed_cmd was never written: the slave keeps its own value
    assert sim.get(FC_HOLDING, 0) == [1234]
    # "initial" values are sent once connected
    wait_until(lambda: sim.get(FC_HOLDING, 20) == [77], message="initial value")
    assert a.client.read("speed_cmd")["value"] is None
    assert a.client.read("speed_cmd")["quality"] == "no_data"


def test_write_inputs_and_encodings(sim, start_ammio):
    a = start_ammio(modbus_config(sim.port))
    c = a.client
    wait_until(lambda: c.value("plc.connected") is True)

    assert c.write("speed_cmd", 500)["ok"]
    assert c.write("temp_cmd", 21.5)["ok"]
    assert c.write("counter_cmd", -2)["ok"]
    assert c.write("stop_cmd", True)["ok"]

    wait_until(lambda: sim.get(FC_HOLDING, 0) == [500], message="uint16 write")
    wait_until(lambda: sim.get(FC_HOLDING, 1, 2) == regs_float(21.5), message="float32 write (ABCD)")
    wait_until(lambda: sim.get(FC_HOLDING, 3, 2) == [0xFFFF, 0xFFFE], message="int32 write")
    wait_until(lambda: sim.get(FC_COILS, 5) == [1], message="coil write")


def test_on_change_writes_do_not_override_the_slave(sim, start_ammio):
    a = start_ammio(modbus_config(sim.port))
    c = a.client
    wait_until(lambda: c.value("plc.connected") is True)
    c.write("speed_cmd", 10)
    wait_until(lambda: sim.get(FC_HOLDING, 0) == [10])
    sim.set(FC_HOLDING, 0, [99])  # the PLC changes the register itself
    time.sleep(0.2)
    assert sim.get(FC_HOLDING, 0) == [99]
    c.write("speed_cmd", 10)  # writing again (even the same value) sends it again
    wait_until(lambda: sim.get(FC_HOLDING, 0) == [10])


def test_cyclic_write_mode(sim, start_ammio):
    a = start_ammio(modbus_config(sim.port, write_mode="cyclic"))
    c = a.client
    wait_until(lambda: c.value("plc.connected") is True)
    c.write("speed_cmd", 10)
    wait_until(lambda: sim.get(FC_HOLDING, 0) == [10])
    sim.set(FC_HOLDING, 0, [99])
    wait_until(lambda: sim.get(FC_HOLDING, 0) == [10], message="cyclic rewrite")


def test_read_outputs_from_all_areas(sim, start_ammio):
    sim.set(FC_HOLDING, 100, [42] + regs_float(3.25) + regs_float(-1.5)[::-1])
    sim.set(FC_DISCRETE, 7, [1])
    sim.set(FC_INPUT_REGS, 10, [0x0001, 0x0002])
    a = start_ammio(modbus_config(sim.port))
    c = a.client
    wait_until(lambda: c.read("speed")["quality"] == "good", message="first poll")
    assert c.value("speed") == 42
    assert c.value("temp") == pytest.approx(3.25)
    assert c.value("temp_swapped") == pytest.approx(-1.5)
    assert c.value("alarm") is True
    assert c.value("uptime") == 0x00010002

    sim.set(FC_HOLDING, 100, [43])
    wait_until(lambda: c.value("speed") == 43, message="updated output")


def test_disconnect_marks_quality_and_reconnects(sim, start_ammio):
    a = start_ammio(modbus_config(sim.port))
    c = a.client
    wait_until(lambda: c.read("speed")["quality"] == "good")
    c.write("speed_cmd", 321)
    wait_until(lambda: sim.get(FC_HOLDING, 0) == [321])

    sim.stop()
    wait_until(lambda: c.value("plc.connected") is False, message="disconnect detected")
    assert c.read("speed")["quality"] == "disconnected"
    assert c.read("speed_cmd")["quality"] == "disconnected"

    # Restart a fresh slave on the same port: inputs with a value are re-sent
    restarted = ModbusSim(sim.port)
    restarted.start()
    try:
        wait_until(lambda: c.value("plc.connected") is True, timeout=10, message="reconnect")
        wait_until(lambda: restarted.get(FC_HOLDING, 0) == [321], message="re-send after reconnect")
        wait_until(lambda: c.read("speed")["quality"] == "good", message="fresh data")
        assert c.read("speed_cmd")["quality"] == "good"
    finally:
        restarted.stop()


def test_device_exception_marks_only_affected_vars_bad(sim, start_ammio):
    cfg = modbus_config(sim.port)
    cfg["plc"]["settings"]["registers"]["outputs"].append(
        {"var_id": "beyond_map", "address": 5000, "type": "uint16"})
    a = start_ammio(cfg)
    c = a.client
    wait_until(lambda: c.read("beyond_map")["quality"] == "bad", message="exception -> bad")
    assert c.value("plc.connected") is True
    assert c.read("speed")["quality"] == "good"

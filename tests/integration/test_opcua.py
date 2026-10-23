import pytest
from asyncua import ua

from conftest import free_port, wait_until
from opcua_sim import OpcuaSim

VARIABLES = {
    "SpeedCmd": (0.0, ua.VariantType.Float),
    "EnableCmd": (False, ua.VariantType.Boolean),
    "Speed": (12.5, ua.VariantType.Float),
    "Count": (7, ua.VariantType.UInt32),
    "AsDouble": (2.75, ua.VariantType.Double),    # read into a float32 variable
    "Wide": (100000, ua.VariantType.Int32),       # read into a uint16 variable: out of range
}


@pytest.fixture
def sim():
    s = OpcuaSim(free_port(), VARIABLES)
    s.start()
    yield s
    s.stop()


def opcua_config(endpoint, read_mode="subscription"):
    return {
        "ua": {
            "protocol": "opcua",
            "settings": {
                "endpoint": endpoint,
                "period_ms": 50,
                "timeout_ms": 2000,
                "read_mode": read_mode,
                "nodes": {
                    "inputs": [
                        {"var_id": "speed_cmd", "node_id": "ns=2;s=SpeedCmd", "type": "float32"},
                        {"var_id": "enable_cmd", "node_id": "ns=2;s=EnableCmd", "type": "bool"},
                    ],
                    "outputs": [
                        {"var_id": "speed", "node_id": "ns=2;s=Speed", "type": "float32"},
                        {"var_id": "count", "node_id": "ns=2;s=Count", "type": "uint32"},
                        {"var_id": "as_double", "node_id": "ns=2;s=AsDouble", "type": "float32"},
                        {"var_id": "wide", "node_id": "ns=2;s=Wide", "type": "uint16"},
                        {"var_id": "missing", "node_id": "ns=2;s=DoesNotExist", "type": "uint16"},
                    ],
                },
            },
        }
    }


@pytest.mark.parametrize("read_mode", ["subscription", "poll"])
def test_read_outputs(sim, start_ammio, read_mode):
    a = start_ammio(opcua_config(sim.endpoint, read_mode))
    c = a.client
    wait_until(lambda: c.read("speed")["quality"] == "good", timeout=15, message="first value")
    assert c.value("speed") == pytest.approx(12.5)
    assert c.value("count") == 7
    assert c.value("as_double") == pytest.approx(2.75)
    # A value that does not fit the configured type is flagged, not silently truncated
    wait_until(lambda: c.read("wide")["quality"] == "bad", message="out-of-range value")
    wait_until(lambda: c.read("missing")["quality"] == "bad", message="unknown node")

    sim.set("Count", 8)
    wait_until(lambda: c.value("count") == 8, message="updated value")


def test_write_inputs(sim, start_ammio):
    a = start_ammio(opcua_config(sim.endpoint))
    c = a.client
    wait_until(lambda: c.value("ua.connected") is True, timeout=15)
    assert c.write("speed_cmd", 33.5)["ok"]
    assert c.write("enable_cmd", True)["ok"]
    wait_until(lambda: sim.get("SpeedCmd") == pytest.approx(33.5), message="float write")
    wait_until(lambda: sim.get("EnableCmd") is True, message="bool write")


def test_server_loss_and_reconnect(sim, start_ammio):
    a = start_ammio(opcua_config(sim.endpoint))
    c = a.client
    wait_until(lambda: c.read("speed")["quality"] == "good", timeout=15)
    c.write("speed_cmd", 5.0)
    wait_until(lambda: sim.get("SpeedCmd") == pytest.approx(5.0))

    sim.stop()
    wait_until(lambda: c.value("ua.connected") is False, timeout=15, message="disconnect detected")
    assert c.read("speed")["quality"] == "disconnected"

    restarted = OpcuaSim(sim.port, VARIABLES)
    restarted.start()
    try:
        wait_until(lambda: c.value("ua.connected") is True, timeout=20, message="reconnect")
        wait_until(lambda: c.read("speed")["quality"] == "good", timeout=10, message="subscription recreated")
        wait_until(lambda: restarted.get("SpeedCmd") == pytest.approx(5.0), message="input re-sent")
    finally:
        restarted.stop()

import json
import time

import pynng
import pytest

from conftest import free_port, wait_until


def offline_config():
    """A Modbus connection to a port nobody listens on: variables exist, the SUT never answers."""
    return {
        "plc": {
            "protocol": "modbus",
            "settings": {
                "host": "127.0.0.1",
                "port": free_port(),
                "period_ms": 50,
                "registers": {
                    "inputs": [
                        {"var_id": "u8", "address": 0, "type": "uint8"},
                        {"var_id": "u16", "address": 1, "type": "uint16"},
                        {"var_id": "i16", "address": 2, "type": "int16"},
                        {"var_id": "f32", "address": 3, "type": "float32"},
                        {"var_id": "flag", "address": 0, "type": "bool", "area": "coil"},
                        {"var_id": "big", "address": 5, "type": "uint64"},
                        {"var_id": "init", "address": 9, "type": "int16", "initial": -5},
                    ],
                    "outputs": [
                        {"var_id": "out", "address": 100, "type": "uint16"},
                    ],
                },
            },
        }
    }


@pytest.fixture
def api(start_ammio):
    return start_ammio(offline_config()).client


def error_code(resp):
    assert resp["ok"] is False, resp
    return resp["error"]["code"]


def test_info(api):
    resp = api.request({"cmd": "info"})
    assert resp["ok"] and resp["name"] == "ammio"
    assert set(resp["protocols"]) == {"modbus", "opcua", "trdp"}
    assert resp["events"] is None


def test_malformed_requests(api):
    assert error_code(api.raw(b"{not json")) == "INVALID_JSON"
    assert error_code(api.raw(b"[1, 2]")) == "INVALID_REQUEST"
    assert error_code(api.request({"var_id": "u8"})) == "INVALID_REQUEST"
    assert error_code(api.request({"cmd": "nope"})) == "UNKNOWN_CMD"
    # Requests are not NUL-terminated on the wire; trailing garbage after the JSON is rejected
    assert error_code(api.raw(b'{"cmd":"info"}xyz')) == "INVALID_JSON"
    # The pre-0.6 list_errors command is gone, but answering it must not break old clients
    assert error_code(api.request({"cmd": "list_errors"})) == "UNKNOWN_CMD"


def test_request_id_is_echoed(api):
    assert api.request({"cmd": "info", "id": 42})["id"] == 42
    assert api.request({"cmd": "nope", "id": "abc"})["id"] == "abc"


def test_read_unknown_and_no_data(api):
    resp = api.request({"cmd": "read", "var_id": "missing"})
    assert error_code(resp) == "NOT_FOUND" and resp["error"]["var_id"] == "missing"

    r = api.read("u16")
    assert r["value"] is None and r["quality"] in ("no_data", "disconnected")
    assert r["type"] == "uint16" and r["dir"] == "input"

    r = api.read("out")
    assert r["value"] is None and r["dir"] == "output"


def test_initial_value(api):
    assert api.value("init") == -5


def test_write_and_read_back(api):
    assert api.write("u16", 65535)["ok"]
    r = api.read("u16")
    assert r["value"] == 65535
    assert isinstance(r["timestamp_ns"], int)
    assert abs(r["timestamp_ns"] / 1e9 - time.time()) < 60

    assert api.write("flag", True)["ok"]
    assert api.value("flag") is True
    assert api.write("flag", 0)["ok"]
    assert api.value("flag") is False

    assert api.write("f32", 1.5)["ok"]
    assert api.value("f32") == 1.5

    # 64-bit integers are carried exactly (no rounding through a double)
    assert api.write("big", 9007199254740991)["ok"]
    assert api.value("big") == 9007199254740991


def test_write_validation(api):
    assert error_code(api.write("out", 1)) == "READ_ONLY"
    assert error_code(api.write("missing", 1)) == "NOT_FOUND"
    assert error_code(api.write("u8", 256)) == "OUT_OF_RANGE"
    assert error_code(api.write("u8", -1)) == "OUT_OF_RANGE"
    assert error_code(api.write("i16", 40000)) == "OUT_OF_RANGE"
    assert error_code(api.write("u16", 1.5)) == "INVALID_VALUE"
    assert error_code(api.write("u16", "12")) == "INVALID_VALUE"
    assert error_code(api.write("flag", 2)) == "OUT_OF_RANGE"
    assert error_code(api.request({"cmd": "write", "var_id": "u16"})) == "INVALID_REQUEST"
    assert api.write("u8", True)["ok"] and api.value("u8") == 1


def test_batch_write_is_all_or_nothing(api):
    assert api.request({"cmd": "write", "values": {"u8": 1, "u16": 2}})["ok"]
    resp = api.request({"cmd": "write", "values": {"u8": 10, "u16": 70000}})
    assert error_code(resp) == "OUT_OF_RANGE" and resp["error"]["var_id"] == "u16"
    assert api.value("u8") == 1, "a rejected batch must not write any value"


def test_batch_read(api):
    api.request({"cmd": "write", "values": {"u8": 7, "i16": -7}})
    resp = api.request({"cmd": "read", "var_ids": ["u8", "i16", "plc.connected"]})
    assert resp["ok"]
    assert [v["var_id"] for v in resp["vars"]] == ["u8", "i16", "plc.connected"]
    assert [v["value"] for v in resp["vars"]] == [7, -7, False]
    assert error_code(api.request({"cmd": "read", "var_ids": ["u8", "nope"]})) == "NOT_FOUND"


def test_listings(api):
    resp = api.request({"cmd": "list_vars"})
    by_id = {v["var_id"]: v for v in resp["vars"]}
    assert by_id["u8"] == {"var_id": "u8", "type": "uint8", "dir": "input", "access": "rw", "connection": "plc"}
    assert by_id["out"]["access"] == "r"
    assert by_id["plc.connected"]["type"] == "bool"

    resp = api.request({"cmd": "list_connections"})
    assert resp["connections"] == [{"name": "plc", "protocol": "modbus", "state": "connecting"}]


def test_events(start_ammio):
    a = start_ammio(offline_config(), events=True)
    assert a.client.request({"cmd": "info"})["events"] == a.events_endpoint

    with pynng.Sub0(dial=a.events_endpoint, recv_timeout=2000) as sub:
        sub.subscribe(b'{"var_id":"u16"')
        # SUB connects asynchronously; keep writing until the first event arrives
        def got_event():
            a.client.write("u16", 5)
            a.client.write("u16", 6)
            try:
                return json.loads(sub.recv().decode())
            except pynng.exceptions.Timeout:
                return None

        event = wait_until(got_event, timeout=10, message="first event")
        assert event["var_id"] == "u16" and event["value"] in (5, 6)
        assert isinstance(event["timestamp_ns"], int)

        a.client.write("u8", 1)       # filtered out by the subscription
        a.client.write("u16", 1234)
        while event["value"] != 1234:
            event = json.loads(sub.recv().decode())
            assert event["var_id"] == "u16"

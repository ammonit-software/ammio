"""TRDP tests: two ammio processes, one playing the SUT side, talk over multicast on a real NIC.

TRDP does not work on the loopback interface, so these tests need a non-loopback IPv4 address
(found automatically, or set AMMIO_TRDP_IP). They are skipped when there is none.
"""

import os
import socket

import pytest

from conftest import wait_until

GROUP_PD = "239.192.10.1"
GROUP_MD = "239.192.10.2"


def local_ip():
    if os.environ.get("AMMIO_TRDP_IP"):
        return os.environ["AMMIO_TRDP_IP"]
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        try:
            s.connect(("192.0.2.1", 9))  # no packet is sent; picks the outgoing interface
            ip = s.getsockname()[0]
        except OSError:
            return None
    return None if ip.startswith("127.") else ip


IP = local_ip()
pytestmark = pytest.mark.skipif(IP is None, reason="no non-loopback IPv4 address for TRDP")

LAYOUT = [
    ("u16", 0, "uint16"),
    ("i32", 16, "int32"),
    ("f64", 48, "float64"),
]


def pd_vars(prefix):
    v = [{"var_id": f"{prefix}_{name}", "offset": off, "type": t} for name, off, t in LAYOUT]
    v.append({"var_id": f"{prefix}_flags", "offset": 112, "type": "bitset16", "bits": ["door_open", "", "fault"]})
    return v


def sender_config():
    return {"tx": {"protocol": "trdp", "settings": {"local_ip": IP, "containers": {
        "inputs": [
            {"id": "STATUS", "comid": 5001, "dest_ip": GROUP_PD, "period_ms": 20, "size_bits": 128,
             "variables": pd_vars("a")},
            {"id": "NOTE", "type": "Mn", "comid": 5101, "dest_ip": GROUP_MD,
             "variables": [{"var_id": "note_code", "type": "uint16"}, {"type": "padding", "bits": 16},
                           {"var_id": "note_value", "type": "float32"}]},
            {"id": "QUERY", "type": "Mr", "comid": 5201, "dest_ip": GROUP_MD, "reply_timeout_ms": 1000,
             "variables": [{"var_id": "query_arg", "type": "uint32"}]},
        ],
        "outputs": [
            {"id": "ANSWER", "type": "Mp", "comid": 5202, "reply_to": "QUERY",
             "structs": {"Entry": [{"var_id": "id", "type": "uint8"}, {"var_id": "level", "type": "int16"}]},
             "variables": [{"var_id": "answer_n", "type": "uint8"},
                           {"var_id": "answer", "type": "Entry", "iterations_id": "answer_count",
                            "max_iterations": 4}]},
        ],
    }}}}


def receiver_config():
    return {"rx": {"protocol": "trdp", "settings": {"local_ip": IP, "containers": {
        "outputs": [
            {"id": "STATUS", "comid": 5001, "multicast_ip": GROUP_PD, "period_ms": 20, "timeout_ms": 200,
             "size_bits": 128, "variables": pd_vars("b")},
            {"id": "NOTE", "type": "Mn", "comid": 5101, "multicast_ip": GROUP_MD,
             "variables": [{"var_id": "got_code", "type": "uint16"}, {"type": "padding", "bits": 16},
                           {"var_id": "got_value", "type": "float32"}]},
            {"id": "QUERY", "type": "Mr", "comid": 5201, "multicast_ip": GROUP_MD,
             "variables": [{"var_id": "got_arg", "type": "uint32"}]},
        ],
        "inputs": [
            {"id": "ANSWER", "type": "Mp", "comid": 5202, "reply_to": "QUERY",
             "structs": {"Entry": [{"var_id": "id", "type": "uint8"}, {"var_id": "level", "type": "int16"}]},
             "variables": [{"var_id": "reply_n", "type": "uint8"},
                           {"var_id": "reply", "type": "Entry", "iterations_id": "reply_count",
                            "max_iterations": 4}]},
        ],
    }}}}


@pytest.fixture
def pair(start_ammio):
    rx = start_ammio(receiver_config())
    tx = start_ammio(sender_config())
    return tx.client, rx.client


def test_process_data(pair):
    tx, rx = pair
    assert rx.read("b_u16")["quality"] == "no_data"  # nothing received yet is not "stale"

    assert tx.request({"cmd": "write", "values": {"a_u16": 513, "a_i32": -70000, "a_f64": 2.5}})["ok"]
    wait_until(lambda: rx.read("b_u16")["quality"] == "good", message="first telegram")
    wait_until(lambda: rx.value("b_i32") == -70000)
    assert rx.value("b_u16") == 513
    assert rx.value("b_f64") == 2.5

    # Bitset members and the whole word stay in sync on both sides; bit 0 is the MSB
    tx.write("a_flags.fault", True)
    wait_until(lambda: rx.value("b_flags.fault") is True)
    assert rx.value("b_flags") == 0x2000
    assert tx.value("a_flags") == 0x2000
    tx.write("a_flags", 0x8000)
    wait_until(lambda: rx.value("b_flags.door_open") is True)
    assert rx.value("b_flags.fault") is False
    assert tx.value("a_flags.door_open") is True and tx.value("a_flags.fault") is False


def test_disable_stops_publishing(pair):
    tx, rx = pair
    tx.write("a_u16", 1)
    wait_until(lambda: rx.read("b_u16")["quality"] == "good")
    tx.write("STATUS.is_enabled", False)
    wait_until(lambda: rx.read("b_u16")["quality"] == "stale", message="timeout after disable")
    assert rx.value("b_u16") == 1  # last value kept
    tx.write("STATUS.is_enabled", True)
    wait_until(lambda: rx.read("b_u16")["quality"] == "good", message="publishing again")


def test_md_notification(pair):
    tx, rx = pair
    tx.request({"cmd": "write", "values": {"note_code": 77, "note_value": 0.5}})
    assert tx.value("NOTE.is_enabled") is False
    tx.write("NOTE.is_enabled", True)  # trigger: sends once and resets
    wait_until(lambda: rx.read("got_code")["quality"] == "good", message="notification")
    assert rx.value("got_code") == 77
    assert rx.value("got_value") == 0.5
    wait_until(lambda: tx.value("NOTE.is_enabled") is False, message="trigger reset")


def test_md_request_reply(pair):
    tx, rx = pair
    # The replying side prepares its answer: 2 entries of the array of structs
    assert rx.request({"cmd": "write", "values": {
        "reply_n": 9, "reply_count": 2,
        "reply[0].id": 1, "reply[0].level": -5,
        "reply[1].id": 2, "reply[1].level": 300,
    }})["ok"]

    tx.write("query_arg", 123456)
    tx.write("QUERY.is_enabled", True)
    wait_until(lambda: tx.read("answer_n")["quality"] == "good", message="reply")
    assert rx.value("got_arg") == 123456
    assert tx.value("answer_n") == 9
    assert tx.value("answer_count") == 2
    assert [tx.value("answer[0].id"), tx.value("answer[0].level")] == [1, -5]
    assert [tx.value("answer[1].id"), tx.value("answer[1].level")] == [2, 300]
    assert tx.read("answer[2].id")["value"] is None


def test_md_reply_timeout(pair):
    tx, rx = pair
    rx.write("ANSWER.is_enabled", False)  # the SUT side stops answering
    tx.write("QUERY.is_enabled", True)
    wait_until(lambda: tx.read("answer_n")["quality"] == "stale", timeout=5, message="reply timeout")

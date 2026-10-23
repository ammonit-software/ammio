"""`ammio --check` must reject every invalid configuration with a message that points at it."""

import subprocess

import pytest

from conftest import REPO_ROOT


def modbus(registers, **settings):
    s = {"host": "127.0.0.1", "registers": registers}
    s.update(settings)
    return {"plc": {"protocol": "modbus", "settings": s}}


def trdp(inputs=(), outputs=(), **settings):
    s = {"local_ip": "0.0.0.0", "containers": {"inputs": list(inputs), "outputs": list(outputs)}}
    s.update(settings)
    return {"net": {"protocol": "trdp", "settings": s}}


def pd(id="C", size_bits=32, variables=None, **kw):
    c = {"id": id, "comid": 1000, "dest_ip": "239.1.1.1", "size_bits": size_bits,
         "variables": variables if variables is not None else [{"var_id": "v", "offset": 0, "type": "uint8"}]}
    c.update(kw)
    return c


@pytest.mark.parametrize("path", sorted((REPO_ROOT / "config").glob("*.example.json")), ids=lambda p: p.name)
def test_examples_are_valid(run_check, path):
    rc, out = run_check(path.read_text())
    assert rc == 0, out


INVALID = {
    "empty file": ({}, "no connections configured"),
    "not json": ("{ // comment\n}", "not valid JSON (near line 1)"),
    "unknown protocol": ({"x": {"protocol": "can", "settings": {}}}, 'unknown protocol "can"'),
    "missing settings": ({"x": {"protocol": "modbus"}}, "missing required key 'settings'"),
    "typo in key": (modbus({"inputs": [{"var_id": "a", "adress": 1, "type": "uint16"}]}), "unknown key 'adress'"),
    "missing address": (modbus({"inputs": [{"var_id": "a", "type": "uint16"}]}), "missing required key 'address'"),
    "missing type": (modbus({"inputs": [{"var_id": "a", "address": 0}]}), "missing required key 'type'"),
    "bad type": (modbus({"inputs": [{"var_id": "a", "address": 0, "type": "uint12"}]}), "not a valid type"),
    "address range": (modbus({"inputs": [{"var_id": "a", "address": 70000, "type": "uint16"}]}), "out of range"),
    "address space end": (modbus({"inputs": [{"var_id": "a", "address": 65535, "type": "uint32"}]}), "does not fit"),
    "overlap": (modbus({"inputs": [{"var_id": "a", "address": 0, "type": "uint32"}],
                        "outputs": [{"var_id": "b", "address": 1, "type": "uint16"}]}), "overlaps"),
    "input in read-only area": (modbus({"inputs": [{"var_id": "a", "address": 0, "type": "uint16",
                                                    "area": "input_register"}]}), "read-only"),
    "coil not bool": (modbus({"outputs": [{"var_id": "a", "address": 0, "type": "uint16", "area": "coil"}]}),
                      "must be \"bool\""),
    "initial out of range": (modbus({"inputs": [{"var_id": "a", "address": 0, "type": "uint8", "initial": 300}]}),
                             "'initial' is out of range"),
    "byte order": (modbus({"inputs": [{"var_id": "a", "address": 0, "type": "uint16"}]}, byte_order="XYZW"),
                   "'byte_order'"),
    "tcp with rtu keys": (modbus({"inputs": [{"var_id": "a", "address": 0, "type": "uint16"}]}, baud=9600),
                          "only valid with \"mode\": \"rtu\""),
    "no registers": (modbus({}), "no inputs or outputs defined"),
    "duplicate var_id across connections": (
        {**modbus({"inputs": [{"var_id": "a", "address": 0, "type": "uint16"}]}),
         "plc2": {"protocol": "modbus", "settings": {"host": "127.0.0.1", "registers": {
             "outputs": [{"var_id": "a", "address": 0, "type": "uint16"}]}}}},
        'duplicate var_id "a"'),
    "opcua bad node id": ({"ua": {"protocol": "opcua", "settings": {"endpoint": "opc.tcp://x:4840", "nodes": {
        "inputs": [{"var_id": "a", "node_id": "Motor.Speed", "type": "float32"}]}}}}, "not a valid NodeId"),
    "opcua user without password": ({"ua": {"protocol": "opcua", "settings": {
        "endpoint": "opc.tcp://x:4840", "username": "u",
        "nodes": {"outputs": [{"var_id": "a", "node_id": "i=85", "type": "uint16"}]}}}}, "must be given together"),
    "trdp variable beyond size": (trdp([pd(variables=[{"var_id": "v", "offset": 16, "type": "uint32"}])]),
                                  "beyond size_bits"),
    "trdp unaligned offset": (trdp([pd(variables=[{"var_id": "v", "offset": 3, "type": "uint8"}])]),
                              "not byte-aligned"),
    "trdp overlap": (trdp([pd(variables=[{"var_id": "a", "offset": 0, "type": "uint16"},
                                         {"var_id": "b", "offset": 8, "type": "uint8"}])]), "overlaps"),
    "trdp size not bytes": (trdp([pd(size_bits=30)]), "not a multiple of 8"),
    "trdp missing dest": (trdp([{k: v for k, v in pd().items() if k != "dest_ip"}]), "missing required key 'dest_ip'"),
    "trdp bad ip": (trdp([pd(dest_ip="239.1.1")]), "not a valid IPv4 address"),
    "trdp unsupported md": (trdp([{"id": "x", "type": "Mq", "comid": 1}]), "not supported"),
    "trdp Mr without Mp": (trdp([{"id": "req", "type": "Mr", "comid": 1, "dest_ip": "10.0.0.1"}]), "has no Mp"),
    "trdp Mp reply_to unknown": (trdp(outputs=[{"id": "rep", "type": "Mp", "comid": 2, "reply_to": "nope"}]),
                                 "does not name an Mr"),
    "trdp bits on scalar": (trdp([pd(variables=[{"var_id": "v", "offset": 0, "type": "uint8", "bits": ["a"]}])]),
                            "only valid for bitset"),
    "trdp duplicate telegram": (trdp([pd(id="A"), pd(id="B", variables=[{"var_id": "w", "offset": 0, "type": "uint8"}])]),
                                "are the same telegram"),
}


@pytest.mark.parametrize("name", INVALID.keys())
def test_invalid_configs_are_rejected(run_check, name):
    config, expected = INVALID[name]
    rc, out = run_check(config)
    assert rc == 1, out
    assert expected in out, out


def test_all_errors_are_reported_at_once(run_check):
    rc, out = run_check(modbus({"inputs": [
        {"var_id": "a", "adress": 1, "type": "uint16"},
        {"var_id": "b", "address": 1, "type": "uint99"},
    ]}))
    assert rc == 1
    assert "unknown key 'adress'" in out and "uint99" in out and "3 error(s)" in out


def test_legacy_keys_still_load_with_warnings(run_check):
    rc, out = run_check({"net": {"interface": "trdp", "specification": {"containers": {"inputs": [
        {"var_id": "C", "comid": 1, "multicast_ip": "239.1.1.1", "size_bits": 8,
         "variables": [{"var_id": "v", "offset": 0, "type": "uint8"}]}]}}}})
    assert rc == 0, out
    assert "'interface' is deprecated" in out
    assert "'specification' is deprecated" in out
    assert "'var_id' on a container is deprecated" in out


def test_cli(ammio_bin):
    r = subprocess.run([str(ammio_bin), "--version"], capture_output=True, text=True)
    assert r.returncode == 0 and r.stdout.startswith("ammio ")
    r = subprocess.run([str(ammio_bin), "--bogus"], capture_output=True, text=True)
    assert r.returncode == 2 and "unknown argument" in r.stderr
    r = subprocess.run([str(ammio_bin), "--config"], capture_output=True, text=True)
    assert r.returncode == 2 and "needs a value" in r.stderr
    r = subprocess.run([str(ammio_bin), "--config", "x.json", "--endpoint", "tcp://127.0.0.1:1",
                        "--log-level", "loud"], capture_output=True, text=True)
    assert r.returncode == 2 and "invalid log level" in r.stderr

"""Source guards for raw Core ownership and hot-path cost decisions."""

import json
from pathlib import Path

import zlink


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src" / "zlink"
COMMON_SPEC = ROOT.parent / "doc" / "spec"


def _runtime_source_files():
    return sorted(SRC.rglob("*.py"))


def test_hot_path_cost_inventory_has_no_unclassified_entries():
    inventory = json.loads(
        (ROOT / "tests" / "hot-path-cost-inventory.json").read_text(encoding="utf-8")
    )
    assert inventory["unclassified"] == []
    assert inventory["entries"]
    for entry in inventory["entries"]:
        assert entry["owner"]
        assert entry["cost_class"] in {"allocation", "copy", "lock", "gil", "none"}
        assert entry["guard_test"]


def test_raw_runtime_does_not_use_dynamic_ffi_or_service_fallbacks():
    text = "\n".join(path.read_text(encoding="utf-8") for path in _runtime_source_files())
    for forbidden in (
        "import cffi",
        "ffi-napi",
        "getattr(lib()",
        "contracts.service",
        "_runtime.service",
        "create_spot_node",
        "send_to_actor",
    ):
        assert forbidden not in text, forbidden


def test_raw_hot_path_keeps_gil_release_and_part_failure_cleanup():
    native_text = (SRC / "_native" / "_zlink_native.c").read_text(encoding="utf-8")
    socket_text = (SRC / "_runtime" / "sockets" / "socket_base.py").read_text(
        encoding="utf-8"
    )
    assert "Py_BEGIN_ALLOW_THREADS" in native_text
    assert "zlink_send_part" in native_text
    assert "zlink_recv_part" in native_text
    assert "zlink_router_recv_part" not in native_text
    assert "_send_payload_via_native_bridge" in socket_text
    assert "_recv_owner_via_native_bridge" in socket_text
    assert "for (Py_ssize_t j = i; j < prepared.count; ++j)" in native_text


def test_public_operations_remain_builder_only():
    assert hasattr(zlink.PairSocket, "send")
    assert hasattr(zlink.DealerSocket, "request")
    assert hasattr(zlink.RouterSocket, "reply")
    assert not hasattr(zlink.PairSocket, "try_send")
    assert not hasattr(zlink.RouterSocket, "send_to_spot")
    assert not hasattr(zlink.StreamSocket, "send_bound_actor")


def test_obsolete_service_examples_are_not_distributed():
    examples = ROOT / "examples"
    assert not (examples / "spot_callback.py").exists()
    assert not (examples / "spot_recv.py").exists()
    for path in examples.glob("*.py"):
        text = path.read_text(encoding="utf-8")
        assert "create_spot_node" not in text, path


def test_common_spec_does_not_reintroduce_removed_raw_channel_name_api():
    for path in (COMMON_SPEC / "README.ko.md", COMMON_SPEC / "README.en.md"):
        text = path.read_text(encoding="utf-8")
        assert "zlink_socket_set_channel_name" not in text, path
        assert "zlink_socket_get_channel_name" not in text, path

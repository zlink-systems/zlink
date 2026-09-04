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
    assert "zlink_router_recv_part" in native_text
    assert "_recv_owner_via_native_bridge" in socket_text
    completion_text = (
        SRC / "_runtime" / "messaging" / "routed_async.py"
    ).read_text(encoding="utf-8")
    assert "class CompletionOwner" in completion_text
    assert "self._entries[entry.context] = entry" in completion_text
    assert "for (Py_ssize_t j = i; j < prepared.count; ++j)" in native_text


def test_async_completion_runtime_uses_blocking_event_wait_without_timer_polling():
    completion_text = (
        SRC / "_runtime" / "messaging" / "routed_async.py"
    ).read_text(encoding="utf-8")
    for forbidden in (
        "asyncio.sleep",
        "call_later",
        "self._runtime_pump",
        "run_in_executor",
    ):
        assert forbidden not in completion_text, forbidden
    assert "target=self._runtime_wait_loop" in completion_text
    assert "daemon=True" in completion_text
    assert "ctypes.byref(native_event),\n                    1,\n                    -1," in completion_text


def test_managed_send_retains_native_messages_instead_of_bytes_snapshots():
    completion_text = (
        SRC / "_runtime" / "messaging" / "routed_async.py"
    ).read_text(encoding="utf-8")
    assert "entry.clone_payload()" in completion_text
    assert "_clone_native_msg(native)" in completion_text
    assert "_snapshot_send_payload" not in completion_text


def test_completion_registry_release_is_constant_time():
    completion_text = (
        SRC / "_runtime" / "messaging" / "routed_async.py"
    ).read_text(encoding="utf-8")
    unregister = completion_text.split("    def _unregister", 1)[1].split(
        "    def ", 1
    )[0]
    assert "self._entries_by_id.get(completion_id)" in unregister
    assert "self._entries_by_id.items()" not in unregister


def test_native_send_builders_do_not_allocate_factory_closures():
    socket_text = (
        SRC / "_runtime" / "sockets" / "socket_base_impl.py"
    ).read_text(encoding="utf-8")
    assert "lambda: _native_socket_send_op_func" not in socket_text
    assert "lambda: _native_routed_send_op_func" not in socket_text


def test_public_operations_remain_builder_only():
    assert hasattr(zlink.PairSocket, "send")
    assert hasattr(zlink.DealerSocket, "request")
    assert hasattr(zlink.RouterSocket, "reply")
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

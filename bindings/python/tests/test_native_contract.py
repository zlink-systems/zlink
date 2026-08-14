import ctypes
import os
from pathlib import Path

from zlink._native import _zlink_native
from zlink._native.ffi import (
    ZlinkMonitorStatus,
    ZlinkMsg,
    ZlinkPollItem,
    ZlinkPollerEvent,
    ZlinkRoutedSendReadyEvent,
    ZlinkRoutedSubmitTarget,
    ZlinkRoutingId,
    ZlinkSocketMonitorOpenOptions,
    lib,
)


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src" / "zlink"


def test_ffi_layouts_are_the_core_0_10_1_layouts():
    assert (ctypes.sizeof(ZlinkMsg), ctypes.alignment(ZlinkMsg)) == (64, 8)
    assert (ctypes.sizeof(ZlinkRoutingId), ctypes.alignment(ZlinkRoutingId)) == (256, 1)
    assert (ctypes.sizeof(ZlinkMonitorStatus), ctypes.alignment(ZlinkMonitorStatus)) == (192, 8)
    assert (ctypes.sizeof(ZlinkSocketMonitorOpenOptions), ctypes.alignment(ZlinkSocketMonitorOpenOptions)) == (16, 8)
    expected_poll_item = (24, 8) if os.name == "nt" else (16, 8)
    assert (ctypes.sizeof(ZlinkPollItem), ctypes.alignment(ZlinkPollItem)) == expected_poll_item
    assert (ctypes.sizeof(ZlinkPollerEvent), ctypes.alignment(ZlinkPollerEvent)) == (48, 8)
    assert (
        ctypes.sizeof(ZlinkRoutedSendReadyEvent),
        ctypes.alignment(ZlinkRoutedSendReadyEvent),
    ) == (280, 8)
    assert (
        ctypes.sizeof(ZlinkRoutedSubmitTarget),
        ctypes.alignment(ZlinkRoutedSubmitTarget),
    ) == (272, 8)


def test_exact_routed_admission_symbols_are_bound_directly():
    native = lib()
    for name in (
        "zlink_routed_send_ready_handler",
        "zlink_select_routed_submit_target",
        "zlink_send_part_transport_pair",
        "zlink_dealer_send_transport_pair_part",
        "zlink_dealer_request_transport_pair_part",
        "zlink_router_request_transport_pair_part",
    ):
        assert callable(getattr(native, name))


def test_native_extension_exposes_only_raw_bridge_operations():
    assert {
        name
        for name in dir(_zlink_native)
        if not name.startswith("__")
    } == {
        "NativeReceivedPartsOwner",
        "BytesReceivedPartsOwner",
        "SocketSendOp",
        "RoutedSendOp",
        "PublisherSendOp",
        "socket_send_op",
        "routed_send_op",
        "publisher_send_op",
        "send_parts",
        "send_parts_rid",
        "publish_parts",
        "recv_parts",
        "recv_owner",
        "recv_retained_owner",
        "dealer_recv_retained_owner",
        "router_recv_owner",
        "router_recv_retained_owner",
        "subscribe_parts",
        "subscribe_owner",
        "subscribe_retained_owner",
    }
    assert not (SRC / "_native" / "_zlink_perf_native.c").exists()
    assert "_zlink_perf_native" not in (ROOT / "setup.py").read_text(encoding="utf-8")


def test_native_source_contains_no_service_ffi_or_repository_fallback():
    source_text = "\n".join(
        path.read_text(encoding="utf-8")
        for path in (
            SRC / "_native" / "ffi.py",
            SRC / "_native" / "_native_loader.py",
            SRC / "_native" / "_zlink_native.c",
        )
    )
    for forbidden in (
        "zlink_spot_",
        "zlink_actor_",
        "zlink_service_",
        "spot_",
        "actor_",
        "core/build",
        "ctypes.util",
    ):
        assert forbidden not in source_text, forbidden
    assert "ZLINK_CORE_PREFIX" in (SRC / "_native" / "_native_loader.py").read_text(
        encoding="utf-8"
    )


def test_package_platform_policy_is_explicit_for_supported_native_targets():
    setup_text = (ROOT / "setup.py").read_text(encoding="utf-8")
    loader_text = (SRC / "_native" / "_native_loader.py").read_text(encoding="utf-8")
    assert "linux-x86_64" in setup_text
    assert "windows-x86_64" in setup_text
    assert "linux-x86_64" in loader_text
    assert "windows-x86_64" in loader_text
    assert "darwin-x86_64" not in setup_text
    assert "linux-aarch64" not in setup_text
    assert "libzlink.dylib" not in loader_text
    assert 'f"native/{platform_dir}/*"' not in setup_text
    native_root = SRC / "native"
    payloads = {
        path.relative_to(native_root).as_posix()
        for path in native_root.rglob("*")
        if path.is_file() or path.is_symlink()
    }
    if os.name == "nt":
        assert "windows-x86_64/zlink.dll" in payloads
    else:
        assert {
            "linux-x86_64/libzlink.so",
            "linux-x86_64/libzlink.so.0",
            "linux-x86_64/libzlink.so.0.11.1",
        }.issubset(payloads)


def test_raw_core_symbol_binding_is_present_and_removed_symbols_are_absent():
    native = lib()
    for name in (
        "zlink_ctx_new",
        "zlink_msg_init",
        "zlink_msg_refcnt",
        "zlink_socket",
        "zlink_send_part",
        "zlink_recv_part",
        "zlink_recv_part_with_hwm_budget_lease",
        "zlink_dealer_recv_part_with_hwm_budget_lease",
        "zlink_router_recv_part",
        "zlink_router_recv_part_v2_with_hwm_budget_lease",
        "zlink_subscribe_part_with_hwm_budget_lease",
        "zlink_hwm_budget_lease_release",
        "zlink_timer_new",
        "zlink_poller_new",
        "zlink_socket_monitor_open",
    ):
        assert hasattr(native, name), name
    for name in (
        "zlink_msg_gets",
        "zlink_spot_new",
        "zlink_actor_new",
        "zlink_service_monitor_open",
    ):
        assert not hasattr(native, name), name

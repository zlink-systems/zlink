import ctypes
import os
from pathlib import Path

from zlink._native import _zlink_native
from zlink._native.ffi import (
    ZlinkCompletion,
    ZlinkMonitorEvent,
    ZlinkMonitorStatus,
    ZlinkMsg,
    ZlinkPollItem,
    ZlinkPollerEvent,
    ZlinkRoutingId,
    ZlinkSocketMonitorOpenOptions,
    lib,
)


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src" / "zlink"


def test_ffi_layouts_are_the_core_0_17_layouts():
    assert (ctypes.sizeof(ZlinkMsg), ctypes.alignment(ZlinkMsg)) == (64, 8)
    assert (ctypes.sizeof(ZlinkRoutingId), ctypes.alignment(ZlinkRoutingId)) == (256, 1)
    assert (ctypes.sizeof(ZlinkCompletion), ctypes.alignment(ZlinkCompletion)) == (312, 8)
    assert (ctypes.sizeof(ZlinkMonitorEvent), ctypes.alignment(ZlinkMonitorEvent)) == (800, 8)
    assert (ctypes.sizeof(ZlinkMonitorStatus), ctypes.alignment(ZlinkMonitorStatus)) == (232, 8)
    assert (ctypes.sizeof(ZlinkSocketMonitorOpenOptions), ctypes.alignment(ZlinkSocketMonitorOpenOptions)) == (16, 8)
    expected_poll_item = (24, 8) if os.name == "nt" else (16, 8)
    assert (ctypes.sizeof(ZlinkPollItem), ctypes.alignment(ZlinkPollItem)) == expected_poll_item
    assert (ctypes.sizeof(ZlinkPollerEvent), ctypes.alignment(ZlinkPollerEvent)) == (48, 8)


def test_pull_completion_and_raw_part_symbols_are_bound_directly():
    native = lib()
    for name in (
        "zlink_send_part",
        "zlink_send_part_rid",
        "zlink_request_part",
        "zlink_reply_part",
        "zlink_router_recv_part",
        "zlink_stream_recv_packet",
        "zlink_completion_recv",
        "zlink_completion_close",
    ):
        assert callable(getattr(native, name))


def test_native_extension_exposes_only_current_raw_bridges():
    names = {name for name in dir(_zlink_native) if not name.startswith("__")}
    assert "dealer_recv_owner" not in names
    assert "TargetSendOp" in names
    assert "router_recv_owner" in names
    assert "recv_owner" in names
    assert "subscribe_owner" in names
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


def test_raw_core_symbol_binding_has_no_framework_service_symbols():
    native = lib()
    for name in (
        "zlink_ctx_new",
        "zlink_msg_init",
        "zlink_msg_refcnt",
        "zlink_socket",
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

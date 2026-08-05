import ctypes
from pathlib import Path

from zlink._native import _zlink_native
from zlink._native.ffi import (
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


def test_ffi_layouts_are_the_core_11_layouts():
    assert (ctypes.sizeof(ZlinkMsg), ctypes.alignment(ZlinkMsg)) == (64, 8)
    assert (ctypes.sizeof(ZlinkRoutingId), ctypes.alignment(ZlinkRoutingId)) == (256, 1)
    assert (ctypes.sizeof(ZlinkMonitorStatus), ctypes.alignment(ZlinkMonitorStatus)) == (232, 8)
    assert (ctypes.sizeof(ZlinkSocketMonitorOpenOptions), ctypes.alignment(ZlinkSocketMonitorOpenOptions)) == (4, 4)
    assert (ctypes.sizeof(ZlinkPollItem), ctypes.alignment(ZlinkPollItem)) == (16, 8)
    assert (ctypes.sizeof(ZlinkPollerEvent), ctypes.alignment(ZlinkPollerEvent)) == (48, 8)


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
        "subscribe_parts",
        "subscribe_owner",
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


def test_package_platform_policy_is_explicit_linux_x86_64_only():
    setup_text = (ROOT / "setup.py").read_text(encoding="utf-8")
    loader_text = (SRC / "_native" / "_native_loader.py").read_text(encoding="utf-8")
    assert 'SUPPORTED_PLATFORM = "linux-x86_64"' in setup_text
    assert 'SUPPORTED_PLATFORM = "linux-x86_64"' in loader_text
    assert "windows-x86_64" not in setup_text
    assert "darwin-x86_64" not in setup_text
    assert "linux-aarch64" not in setup_text
    assert "_prepare_windows_runtime" not in loader_text
    assert "libzlink.dylib" not in loader_text
    assert 'f"native/{platform_dir}/*"' not in setup_text
    native_root = SRC / "native"
    assert {
        path.relative_to(native_root).as_posix()
        for path in native_root.rglob("*")
        if path.is_file() or path.is_symlink()
    } == {
        "linux-x86_64/libzlink.so",
        "linux-x86_64/libzlink.so.11",
        "linux-x86_64/libzlink.so.11.2.0",
    }


def test_raw_core_symbol_binding_is_present_and_removed_symbols_are_absent():
    native = lib()
    for name in (
        "zlink_ctx_new",
        "zlink_msg_init",
        "zlink_msg_refcnt",
        "zlink_socket",
        "zlink_send_part",
        "zlink_recv_part",
        "zlink_router_recv_part",
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

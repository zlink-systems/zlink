# SPDX-License-Identifier: MPL-2.0

import ctypes
import os

from ._native_loader import load_native_library


class ZlinkMsg(ctypes.Union):
    """The opaque 64-byte ``zlink_msg_t`` storage used by Core."""

    _fields_ = [
        ("_", ctypes.c_ubyte * 64),
        ("_align", ctypes.c_void_p),
    ]


class ZlinkRoutingId(ctypes.Structure):
    _fields_ = [("size", ctypes.c_uint8), ("data", ctypes.c_uint8 * 255)]


class ZlinkMonitorEvent(ctypes.Structure):
    _fields_ = [
        ("event", ctypes.c_uint64),
        ("value", ctypes.c_uint64),
        ("routing_id", ZlinkRoutingId),
        ("local_addr", ctypes.c_char * 256),
        ("remote_addr", ctypes.c_char * 256),
    ]


class ZlinkSocketMonitorOpenOptions(ctypes.Structure):
    _fields_ = [("events", ctypes.c_uint32)]


class ZlinkMonitorStatus(ctypes.Structure):
    _fields_ = [
        ("abi_version", ctypes.c_uint32),
        ("struct_size", ctypes.c_uint32),
        ("source_kind", ctypes.c_uint32),
        ("state_flags", ctypes.c_uint32),
        ("detail_flags", ctypes.c_uint32),
        ("snd_pending_msgs", ctypes.c_uint64),
        ("rcv_pending_msgs", ctypes.c_uint64),
        ("auto_hwm_enabled", ctypes.c_uint32),
        ("auto_hwm_profile", ctypes.c_uint32),
        ("auto_hwm_role", ctypes.c_uint32),
        ("auto_hwm_policy_class", ctypes.c_uint32),
        ("auto_hwm_unit_budget_bytes", ctypes.c_uint64),
        ("auto_hwm_size_cap", ctypes.c_uint32),
        ("auto_hwm_socket_message_slots", ctypes.c_uint64),
        ("auto_hwm_connection_bucket_enabled", ctypes.c_uint32),
        ("auto_hwm_connection_bucket_count", ctypes.c_uint32),
        ("auto_hwm_connection_bucket_index", ctypes.c_uint32),
        ("auto_hwm_connection_bucket_hwm_4k", ctypes.c_uint32),
        ("auto_hwm_connection_bucket_hysteresis_retained", ctypes.c_uint32),
        ("auto_hwm_effective_message_bytes", ctypes.c_uint64),
        ("auto_hwm_planned_sndhwm_bytes", ctypes.c_uint64),
        ("auto_hwm_planned_rcvhwm_bytes", ctypes.c_uint64),
        ("auto_hwm_applied_sndhwm_bytes", ctypes.c_uint64),
        ("auto_hwm_applied_rcvhwm_bytes", ctypes.c_uint64),
        ("auto_hwm_effective_sndbuf", ctypes.c_int32),
        ("auto_hwm_effective_rcvbuf", ctypes.c_int32),
        ("auto_hwm_last_recalc_ms", ctypes.c_uint64),
        ("auto_hwm_last_recalc_reason", ctypes.c_uint32),
        ("auto_hwm_send_blocked_ratio_ppm", ctypes.c_uint32),
        ("auto_hwm_deferred_sndhwm_bytes", ctypes.c_uint64),
        ("auto_hwm_deferred_rcvhwm_bytes", ctypes.c_uint64),
        ("auto_hwm_deferred_sndhwm_valid", ctypes.c_uint32),
        ("auto_hwm_deferred_rcvhwm_valid", ctypes.c_uint32),
        ("snd_bytes_in_flight", ctypes.c_uint64),
        ("rcv_bytes_in_flight", ctypes.c_uint64),
        ("minimum_core_message_charge_bytes", ctypes.c_uint64),
        ("oversize_message_admission_count", ctypes.c_uint64),
        ("oversize_message_admission_max_bytes", ctypes.c_uint64),
    ]


if os.name == "nt":
    ZlinkFD = ctypes.c_ulonglong if ctypes.sizeof(ctypes.c_void_p) == 8 else ctypes.c_uint
else:
    ZlinkFD = ctypes.c_int


class ZlinkPollItem(ctypes.Structure):
    _fields_ = [
        ("socket", ctypes.c_void_p),
        ("fd", ZlinkFD),
        ("events", ctypes.c_short),
        ("revents", ctypes.c_short),
    ]


class ZlinkPollerEvent(ctypes.Structure):
    _fields_ = [
        ("source_kind", ctypes.c_uint32),
        ("socket", ctypes.c_void_p),
        ("fd", ZlinkFD),
        ("timer", ctypes.c_void_p),
        ("user_data", ctypes.c_void_p),
        ("events", ctypes.c_short),
    ]


ZLINK_PART_FINAL = 0
ZLINK_PART_MORE = 1
ZLINK_DONTWAIT = 1


class _Lib:
    """Own the complete Core 11 raw symbol and layout declaration."""

    def __init__(self):
        self.lib = load_native_library(self._bind_loaded)

    def _bind_loaded(self, loaded):
        self.lib = loaded
        self._bind()

    def _require(self, name, argtypes, restype):
        function = getattr(self.lib, name)
        function.argtypes = argtypes
        function.restype = restype
        return function

    def _bind(self):
        p_msg = ctypes.POINTER(ZlinkMsg)
        p_rid = ctypes.POINTER(ZlinkRoutingId)
        p_rid_ptr = ctypes.POINTER(p_rid)
        p_int = ctypes.POINTER(ctypes.c_int)
        p_size = ctypes.POINTER(ctypes.c_size_t)

        specs = [
            ("zlink_version", [p_int, p_int, p_int], None),
            ("zlink_errno", [], ctypes.c_int),
            ("zlink_strerror", [ctypes.c_int], ctypes.c_char_p),
            ("zlink_ctx_new", [], ctypes.c_void_p),
            ("zlink_ctx_term", [ctypes.c_void_p], ctypes.c_int),
            ("zlink_ctx_shutdown", [ctypes.c_void_p], ctypes.c_int),
            ("zlink_ctx_set", [ctypes.c_void_p, ctypes.c_int, ctypes.c_int], ctypes.c_int),
            ("zlink_ctx_set_data", [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t], ctypes.c_int),
            ("zlink_ctx_get_data", [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, p_size], ctypes.c_int),
            ("zlink_ctx_get", [ctypes.c_void_p, ctypes.c_int, p_int], ctypes.c_int),
            ("zlink_ctx_auto_hwm_recalculate", [ctypes.c_void_p], ctypes.c_int),
            ("zlink_proxy", [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p], ctypes.c_int),
            ("zlink_proxy_steerable", [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p], ctypes.c_int),
            ("zlink_has", [ctypes.c_char_p], ctypes.c_bool),
            ("zlink_atomic_counter_new", [], ctypes.c_void_p),
            ("zlink_atomic_counter_set", [ctypes.c_void_p, ctypes.c_int], None),
            ("zlink_atomic_counter_inc", [ctypes.c_void_p], ctypes.c_int),
            ("zlink_atomic_counter_dec", [ctypes.c_void_p], ctypes.c_int),
            ("zlink_atomic_counter_value", [ctypes.c_void_p], ctypes.c_int),
            ("zlink_atomic_counter_destroy", [ctypes.POINTER(ctypes.c_void_p)], None),
            ("zlink_stopwatch_start", [], ctypes.c_void_p),
            ("zlink_stopwatch_intermediate", [ctypes.c_void_p], ctypes.c_ulong),
            ("zlink_stopwatch_stop", [ctypes.c_void_p], ctypes.c_ulong),
            ("zlink_sleep", [ctypes.c_int], None),
            ("zlink_thread_start", [ctypes.c_void_p, ctypes.c_void_p], ctypes.c_void_p),
            ("zlink_thread_join", [ctypes.c_void_p], None),
            ("zlink_msg_init", [p_msg], ctypes.c_int),
            ("zlink_msg_init_size", [p_msg, ctypes.c_size_t], ctypes.c_int),
            ("zlink_msg_init_data", [p_msg, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_void_p], ctypes.c_int),
            ("zlink_msg_close", [p_msg], ctypes.c_int),
            ("zlink_msg_move", [p_msg, p_msg], ctypes.c_int),
            ("zlink_msg_copy", [p_msg, p_msg], ctypes.c_int),
            ("zlink_msg_adopt", [p_msg, p_msg], ctypes.c_int),
            ("zlink_msg_data", [p_msg], ctypes.c_void_p),
            ("zlink_msg_size", [p_msg], ctypes.c_size_t),
            ("zlink_msg_refcnt", [p_msg, p_int], ctypes.c_int),
            ("zlink_multipart_close", [p_msg, ctypes.c_size_t], None),
            ("zlink_socket", [ctypes.c_void_p, ctypes.c_int], ctypes.c_void_p),
            ("zlink_recv_handler", [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p], ctypes.c_int),
            ("zlink_stream_packet_handler", [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p], ctypes.c_int),
            ("zlink_send_ready_handler", [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p], ctypes.c_int),
            ("zlink_router_completion_control_handler", [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p], ctypes.c_int),
            ("zlink_close", [ctypes.c_void_p], ctypes.c_int),
            ("zlink_set_option", [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t], ctypes.c_int),
            ("zlink_get_option", [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, p_size], ctypes.c_int),
            ("zlink_set_routing_id", [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t], ctypes.c_int),
            ("zlink_get_routing_id", [ctypes.c_void_p, p_rid], ctypes.c_int),
            ("zlink_set_tls_server", [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int], ctypes.c_int),
            ("zlink_set_tls_client", [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int], ctypes.c_int),
            ("zlink_set_router_option", [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t], ctypes.c_int),
            ("zlink_get_router_option", [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, p_size], ctypes.c_int),
            ("zlink_set_dealer_option", [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t], ctypes.c_int),
            ("zlink_get_dealer_option", [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, p_size], ctypes.c_int),
            ("zlink_set_stream_option", [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t], ctypes.c_int),
            ("zlink_get_stream_option", [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, p_size], ctypes.c_int),
            ("zlink_set_pub_option", [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t], ctypes.c_int),
            ("zlink_get_pub_option", [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, p_size], ctypes.c_int),
            ("zlink_set_sub_option", [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t], ctypes.c_int),
            ("zlink_get_sub_option", [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, p_size], ctypes.c_int),
            ("zlink_bind", [ctypes.c_void_p, ctypes.c_char_p], ctypes.c_int),
            ("zlink_connect", [ctypes.c_void_p, ctypes.c_char_p], ctypes.c_int),
            ("zlink_unbind", [ctypes.c_void_p, ctypes.c_char_p], ctypes.c_int),
            ("zlink_disconnect", [ctypes.c_void_p, ctypes.c_char_p], ctypes.c_int),
            ("zlink_disconnect_rid", [ctypes.c_void_p, p_rid], ctypes.c_int),
            ("zlink_send_part", [ctypes.c_void_p, p_msg, ctypes.c_int, ctypes.c_int], ctypes.c_int),
            ("zlink_send_part_rid", [ctypes.c_void_p, p_rid, p_msg, ctypes.c_int, ctypes.c_int], ctypes.c_int),
            ("zlink_dealer_request_part", [ctypes.c_void_p, p_msg, ctypes.c_int, ctypes.c_int, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_void_p], ctypes.c_int),
            ("zlink_dealer_recv_part", [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint8), ctypes.POINTER(ctypes.c_uint64), p_msg, p_int, ctypes.c_int], ctypes.c_int),
            ("zlink_dealer_reply_part", [ctypes.c_void_p, ctypes.c_uint64, p_msg, ctypes.c_int], ctypes.c_int),
            ("zlink_router_request_part", [ctypes.c_void_p, p_rid, p_msg, ctypes.c_int, ctypes.c_int, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_void_p], ctypes.c_int),
            ("zlink_router_reply_part", [ctypes.c_void_p, p_rid, ctypes.c_uint64, p_msg, ctypes.c_int], ctypes.c_int),
            ("zlink_router_completion_control_part", [ctypes.c_void_p, p_rid, p_msg, ctypes.c_int], ctypes.c_int),
            ("zlink_router_recv_part", [ctypes.c_void_p, p_rid_ptr, ctypes.POINTER(ctypes.c_uint64), p_msg, p_int, ctypes.c_int], ctypes.c_int),
            ("zlink_recv_part", [ctypes.c_void_p, p_rid_ptr, p_msg, p_int, ctypes.c_int], ctypes.c_int),
            ("zlink_publish_part", [ctypes.c_void_p, ctypes.c_char_p, p_msg, ctypes.c_int, ctypes.c_int], ctypes.c_int),
            ("zlink_set_subscription", [ctypes.c_void_p, ctypes.c_char_p], ctypes.c_int),
            ("zlink_unset_subscription", [ctypes.c_void_p, ctypes.c_char_p], ctypes.c_int),
            ("zlink_subscription_at", [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, p_size, p_int], ctypes.c_int),
            ("zlink_subscribe_part", [ctypes.c_void_p, p_rid_ptr, ctypes.POINTER(ctypes.c_char), ctypes.c_size_t, p_size, p_msg, p_int, ctypes.c_int], ctypes.c_int),
            ("zlink_xpub_recv_part", [ctypes.c_void_p, p_rid_ptr, p_int, ctypes.POINTER(ctypes.c_char), ctypes.c_size_t, p_size, ctypes.c_int], ctypes.c_int),
            ("zlink_monitor_ignore_handler", [ctypes.POINTER(ZlinkMonitorEvent), ctypes.c_void_p], None),
            ("zlink_socket_monitor_open", [ctypes.c_void_p, ctypes.POINTER(ZlinkSocketMonitorOpenOptions)], ctypes.c_void_p),
            ("zlink_socket_monitor_handler", [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p], ctypes.c_int),
            ("zlink_socket_monitor_recv", [ctypes.c_void_p, ctypes.POINTER(ZlinkMonitorEvent), ctypes.c_int], ctypes.c_int),
            ("zlink_monitor_status", [ctypes.c_void_p, ctypes.POINTER(ZlinkMonitorStatus)], ctypes.c_int),
            ("zlink_monitor_close", [ctypes.POINTER(ctypes.c_void_p)], ctypes.c_int),
            ("zlink_poll", [ctypes.POINTER(ZlinkPollItem), ctypes.c_int, ctypes.c_long, p_int], ctypes.c_int),
            ("zlink_poller_new", [], ctypes.c_void_p),
            ("zlink_poller_destroy", [ctypes.POINTER(ctypes.c_void_p)], ctypes.c_int),
            ("zlink_poller_size", [ctypes.c_void_p, p_int], ctypes.c_int),
            ("zlink_poller_add", [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_short], ctypes.c_int),
            ("zlink_poller_modify", [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_short], ctypes.c_int),
            ("zlink_poller_remove", [ctypes.c_void_p, ctypes.c_void_p], ctypes.c_int),
            ("zlink_poller_add_fd", [ctypes.c_void_p, ZlinkFD, ctypes.c_void_p, ctypes.c_short], ctypes.c_int),
            ("zlink_poller_add_timer", [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p], ctypes.c_int),
            ("zlink_poller_modify_fd", [ctypes.c_void_p, ZlinkFD, ctypes.c_short], ctypes.c_int),
            ("zlink_poller_remove_fd", [ctypes.c_void_p, ZlinkFD], ctypes.c_int),
            ("zlink_poller_remove_timer", [ctypes.c_void_p, ctypes.c_void_p], ctypes.c_int),
            ("zlink_poller_wait", [ctypes.c_void_p, ctypes.POINTER(ZlinkPollerEvent), ctypes.c_int, ctypes.c_long, p_int], ctypes.c_int),
            ("zlink_timer_new", [], ctypes.c_void_p),
            ("zlink_timer_destroy", [ctypes.POINTER(ctypes.c_void_p)], ctypes.c_int),
            ("zlink_timer_start", [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint64], ctypes.c_int),
            ("zlink_timer_stop", [ctypes.c_void_p], ctypes.c_int),
            ("zlink_timer_recv", [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint64)], ctypes.c_int),
            ("zlink_timer_handler", [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p], ctypes.c_int),
        ]
        for name, argtypes, restype in specs:
            self._require(name, argtypes, restype)


_lib = None


def lib():
    global _lib
    if _lib is None:
        _lib = _Lib()
    return _lib.lib

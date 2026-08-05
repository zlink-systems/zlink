//! Raw FFI declarations for the Core 11 public header set.
//!
//! This module is crate-private. The declarations intentionally contain only
//! symbols present in the candidate `zlink.h` headers copied into this crate;
//! higher layers must not add compatibility declarations for removed APIs.

#![allow(non_camel_case_types, non_upper_case_globals, dead_code)]

use std::ffi::{c_char, c_int, c_long, c_ulong, c_void};

// ---------------------------------------------------------------------------
// Common ABI types
// ---------------------------------------------------------------------------

#[repr(C, align(8))]
#[derive(Copy, Clone)]
pub struct zlink_msg_t {
    _data: [u64; 8],
}

#[repr(C)]
#[derive(Copy, Clone, Debug, PartialEq, Eq, Hash)]
pub struct zlink_routing_id_t {
    pub size: u8,
    pub data: [u8; 255],
}

impl zlink_routing_id_t {
    pub(crate) const fn empty() -> Self {
        Self {
            size: 0,
            data: [0; 255],
        }
    }
}

// ---------------------------------------------------------------------------
// Enums and flags
// ---------------------------------------------------------------------------

#[repr(C)]
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum zlink_ctx_option_t {
    ZLINK_IO_THREADS = 1,
    ZLINK_MAX_SOCKETS = 2,
    ZLINK_SOCKET_LIMIT = 3,
    ZLINK_THREAD_SCHED_POLICY = 4,
    ZLINK_MAX_MSGSZ = 5,
    ZLINK_MSG_T_SIZE = 6,
    ZLINK_THREAD_AFFINITY_CPU_ADD = 7,
    ZLINK_THREAD_AFFINITY_CPU_REMOVE = 8,
    ZLINK_THREAD_NAME_PREFIX = 9,
    ZLINK_CTX_OPT_BLOCKY = 10,
    ZLINK_CTX_OPT_AUTO_HWM_ENABLE = 12,
    ZLINK_CTX_OPT_AUTO_HWM_RECALC_DEBOUNCE_MS = 14,
    ZLINK_CTX_OPT_AUTO_HWM_PROFILE = 17,
    ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES = 18,
}

impl zlink_ctx_option_t {
    pub const ZLINK_THREAD_PRIORITY: Self = Self::ZLINK_SOCKET_LIMIT;
}

#[repr(C)]
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum zlink_auto_hwm_profile_t {
    ZLINK_AUTO_HWM_PROFILE_COMPACT = 0,
    ZLINK_AUTO_HWM_PROFILE_LOW_LATENCY = 1,
    ZLINK_AUTO_HWM_PROFILE_BALANCED = 2,
    ZLINK_AUTO_HWM_PROFILE_THROUGHPUT = 3,
}

#[repr(C)]
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum zlink_socket_type_t {
    ZLINK_SOCKET_ANY = 0,
    ZLINK_SOCKET_PAIR = 0x1001,
    ZLINK_SOCKET_PUB = 0x1002,
    ZLINK_SOCKET_SUB = 0x1003,
    ZLINK_SOCKET_DEALER = 0x1004,
    ZLINK_SOCKET_ROUTER = 0x1005,
    ZLINK_SOCKET_XPUB = 0x1006,
    ZLINK_SOCKET_XSUB = 0x1007,
    ZLINK_SOCKET_STREAM = 0x1008,
}

#[repr(C)]
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum zlink_option_t {
    ZLINK_OPT_AFFINITY = 0x3001,
    ZLINK_OPT_RATE = 0x3003,
    ZLINK_OPT_RECOVERY_IVL = 0x3004,
    ZLINK_OPT_SNDBUF = 0x3005,
    ZLINK_OPT_RCVBUF = 0x3006,
    ZLINK_OPT_MAXMSGSIZE = 0x300E,
    ZLINK_OPT_SNDHWM = 0x300F,
    ZLINK_OPT_RCVHWM = 0x3010,
    ZLINK_OPT_LINGER = 0x300A,
    ZLINK_OPT_RECONNECT_IVL = 0x300B,
    ZLINK_OPT_BACKLOG = 0x300C,
    ZLINK_OPT_RECONNECT_IVL_MAX = 0x300D,
    ZLINK_OPT_MULTICAST_HOPS = 0x3011,
    ZLINK_OPT_RCVTIMEO = 0x3012,
    ZLINK_OPT_SNDTIMEO = 0x3013,
    ZLINK_OPT_CONNECT_TIMEOUT = 0x3024,
    ZLINK_OPT_HANDSHAKE_IVL = 0x301D,
    ZLINK_OPT_TCP_KEEPALIVE = 0x3015,
    ZLINK_OPT_TCP_KEEPALIVE_CNT = 0x3016,
    ZLINK_OPT_TCP_KEEPALIVE_IDLE = 0x3017,
    ZLINK_OPT_TCP_KEEPALIVE_INTVL = 0x3018,
    ZLINK_OPT_TCP_MAXRT = 0x3025,
    ZLINK_OPT_TCP_NODELAY = 0x3031,
    ZLINK_OPT_IPV6 = 0x301A,
    ZLINK_OPT_TOS = 0x301C,
    ZLINK_OPT_MULTICAST_MAXTPDU = 0x3026,
    ZLINK_OPT_BINDTODEVICE = 0x3027,
    ZLINK_OPT_TLS_CERT = 0x3028,
    ZLINK_OPT_TLS_KEY = 0x3029,
    ZLINK_OPT_TLS_CA = 0x302A,
    ZLINK_OPT_TLS_VERIFY = 0x302B,
    ZLINK_OPT_TLS_REQUIRE_CLIENT_CERT = 0x302C,
    ZLINK_OPT_TLS_HOSTNAME = 0x302D,
    ZLINK_OPT_TLS_TRUST_SYSTEM = 0x302E,
    ZLINK_OPT_TLS_PASSWORD = 0x302F,
    ZLINK_OPT_IMMEDIATE = 0x3019,
    ZLINK_OPT_CONFLATE = 0x301B,
    ZLINK_OPT_BLOCKY = 0x301E,
    ZLINK_OPT_INVERT_MATCHING = 0x3020,
    ZLINK_OPT_SUBMIT_RETRY_MODE = 0x3037,
    ZLINK_OPT_SUBMIT_RETRY_TIMEOUT = 0x3038,
    ZLINK_OPT_SUBMIT_RETRY_ATTEMPTS = 0x3039,
    ZLINK_OPT_FD = 0x3007,
    ZLINK_OPT_EVENTS = 0x3008,
    ZLINK_OPT_TYPE = 0x3009,
    ZLINK_OPT_LAST_ENDPOINT = 0x3014,
    ZLINK_OPT_ZMP_METADATA = 0x3030,
    ZLINK_OPT_ROUTE_VALUE_MAX_SIZE = 0x3032,
    ZLINK_OPT_RID_DUPLICATE_POLICY = 0x3033,
    ZLINK_OPT_AUTO_HWM_MSG_UNIT_BYTES = 0x3034,
}

#[repr(C)]
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum zlink_router_option_t {
    ZLINK_ROUTER_OPT_MANDATORY = 0x3101,
    ZLINK_ROUTER_OPT_PROBE = 0x3103,
    ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID = 0x3104,
    ZLINK_ROUTER_OPT_REQUEST_TIMEOUT_MS = 0x3105,
    ZLINK_ROUTER_OPT_WEIGHT = 0x3106,
}

#[repr(C)]
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum zlink_dealer_option_t {
    ZLINK_DEALER_OPT_PROBE = 0x3201,
    ZLINK_DEALER_OPT_REQUEST_TIMEOUT_MS = 0x3202,
    ZLINK_DEALER_OPT_WEIGHT = 0x3203,
}

#[repr(C)]
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum zlink_pub_option_t {
    ZLINK_PUB_OPT_VERBOSE = 0x3301,
    ZLINK_PUB_OPT_VERBOSER = 0x3302,
    ZLINK_PUB_OPT_MANUAL = 0x3303,
    ZLINK_PUB_OPT_MANUAL_LAST_VALUE = 0x3304,
    ZLINK_PUB_OPT_NODROP = 0x3305,
    ZLINK_PUB_OPT_WELCOME_MSG = 0x3306,
    ZLINK_PUB_OPT_TOPICS_COUNT = 0x3307,
    ZLINK_PUB_OPT_APPROVE_SUBSCRIBE = 0x3308,
    ZLINK_PUB_OPT_REJECT_SUBSCRIBE = 0x3309,
}

#[repr(C)]
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum zlink_sub_option_t {
    ZLINK_SUB_OPT_TOPICS_COUNT = 0x3400,
}

#[repr(C)]
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum zlink_stream_option_t {
    ZLINK_STREAM_OPT_NOTIFY = 0x3501,
}

pub type zlink_send_flags_t = u32;
pub type zlink_recv_flags_t = u32;
pub const ZLINK_DONTWAIT: zlink_send_flags_t = 0x0001;

#[repr(C)]
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum zlink_part_flag_t {
    ZLINK_PART_FINAL = 0,
    ZLINK_PART_MORE = 1,
}

#[repr(C)]
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum zlink_request_result_t {
    ZLINK_REQUEST_RESULT_OK = 0,
    ZLINK_REQUEST_RESULT_TIMED_OUT = 101,
    ZLINK_REQUEST_RESULT_NOT_FOUND = 102,
    ZLINK_REQUEST_RESULT_TERMINATED = 103,
    ZLINK_REQUEST_RESULT_PROTOCOL_ERROR = 104,
    ZLINK_REQUEST_RESULT_INTERNAL_ERROR = 105,
    ZLINK_REQUEST_RESULT_REJECTED = 106,
    ZLINK_REQUEST_RESULT_CONFLICT = 107,
    ZLINK_REQUEST_RESULT_BUSY = 108,
    ZLINK_REQUEST_RESULT_NOT_CONNECTED = 109,
    ZLINK_REQUEST_RESULT_INVALID_ARGUMENT = 110,
    ZLINK_REQUEST_RESULT_INVALID_STATE = 111,
    ZLINK_REQUEST_RESULT_NOT_SUPPORTED = 112,
}

#[repr(C)]
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum zlink_config_result_t {
    ZLINK_CONFIG_OK = 0,
    ZLINK_CONFIG_INVALID_HANDLE = 701,
    ZLINK_CONFIG_INVALID_ARGUMENT = 702,
    ZLINK_CONFIG_NOT_SUPPORTED = 703,
    ZLINK_CONFIG_INTERNAL_ERROR = 704,
    ZLINK_CONFIG_INVALID_STATE = 705,
    ZLINK_CONFIG_NOT_FOUND = 706,
}

#[repr(C)]
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum zlink_monitor_source_kind_t {
    ZLINK_MONITOR_SOURCE_SOCKET = 1,
}

pub type zlink_socket_monitor_event_mask_t = u32;
pub type zlink_monitor_state_mask_t = u32;
pub type zlink_monitor_status_detail_mask_t = u32;
pub const ZLINK_MONITOR_STATE_READY: u32 = 1 << 0;
pub const ZLINK_MONITOR_STATE_CLOSED: u32 = 1 << 3;

#[repr(C)]
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum zlink_poller_source_kind_t {
    ZLINK_POLLER_SOURCE_SOCKET = 1,
    ZLINK_POLLER_SOURCE_FD = 2,
    ZLINK_POLLER_SOURCE_TIMER = 3,
}

#[cfg(windows)]
pub type zlink_fd_t = usize;
#[cfg(not(windows))]
pub type zlink_fd_t = c_int;

// ---------------------------------------------------------------------------
// Monitor and poller records
// ---------------------------------------------------------------------------

#[repr(C)]
#[derive(Copy, Clone)]
pub struct zlink_monitor_event_t {
    pub event: u64,
    pub value: u64,
    pub routing_id: zlink_routing_id_t,
    pub local_addr: [c_char; 256],
    pub remote_addr: [c_char; 256],
}

pub type zlink_socket_monitor_event_t = zlink_monitor_event_t;

#[repr(C)]
#[derive(Copy, Clone)]
pub struct zlink_socket_monitor_open_options_t {
    pub events: zlink_socket_monitor_event_mask_t,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct zlink_monitor_status_t {
    pub abi_version: u32,
    pub struct_size: u32,
    pub source_kind: zlink_monitor_source_kind_t,
    pub state_flags: zlink_monitor_state_mask_t,
    pub detail_flags: zlink_monitor_status_detail_mask_t,
    pub snd_pending_msgs: u64,
    pub rcv_pending_msgs: u64,
    pub auto_hwm_enabled: u32,
    pub auto_hwm_profile: u32,
    pub auto_hwm_role: u32,
    pub auto_hwm_policy_class: u32,
    pub auto_hwm_unit_budget_bytes: u64,
    pub auto_hwm_size_cap: u32,
    pub auto_hwm_socket_message_slots: u64,
    pub auto_hwm_connection_bucket_enabled: u32,
    pub auto_hwm_connection_bucket_count: u32,
    pub auto_hwm_connection_bucket_index: u32,
    pub auto_hwm_connection_bucket_hwm_4k: u32,
    pub auto_hwm_connection_bucket_hysteresis_retained: u32,
    pub auto_hwm_effective_message_bytes: u64,
    pub auto_hwm_planned_sndhwm_bytes: u64,
    pub auto_hwm_planned_rcvhwm_bytes: u64,
    pub auto_hwm_applied_sndhwm_bytes: u64,
    pub auto_hwm_applied_rcvhwm_bytes: u64,
    pub auto_hwm_effective_sndbuf: i32,
    pub auto_hwm_effective_rcvbuf: i32,
    pub auto_hwm_last_recalc_ms: u64,
    pub auto_hwm_last_recalc_reason: u32,
    pub auto_hwm_send_blocked_ratio_ppm: u32,
    pub auto_hwm_deferred_sndhwm_bytes: u64,
    pub auto_hwm_deferred_rcvhwm_bytes: u64,
    pub auto_hwm_deferred_sndhwm_valid: u32,
    pub auto_hwm_deferred_rcvhwm_valid: u32,
    pub snd_bytes_in_flight: u64,
    pub rcv_bytes_in_flight: u64,
    pub minimum_core_message_charge_bytes: u64,
    pub oversize_message_admission_count: u64,
    pub oversize_message_admission_max_bytes: u64,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct zlink_pollitem_t {
    pub socket: *mut c_void,
    pub fd: zlink_fd_t,
    pub events: i16,
    pub revents: i16,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct zlink_poller_event_t {
    pub source_kind: zlink_poller_source_kind_t,
    pub socket: *mut c_void,
    pub fd: zlink_fd_t,
    pub timer: *mut c_void,
    pub user_data: *mut c_void,
    pub events: i16,
}

// ---------------------------------------------------------------------------
// Callback types
// ---------------------------------------------------------------------------

pub type zlink_stream_packet_handler_fn = unsafe extern "C" fn(
    stream: *mut c_void,
    source_rid: *const zlink_routing_id_t,
    header: *mut zlink_msg_t,
    body: *mut zlink_msg_t,
    userdata: *mut c_void,
);

pub type zlink_send_ready_handler_fn =
    unsafe extern "C" fn(subject: *mut c_void, userdata: *mut c_void);

pub type zlink_reply_handler_fn = unsafe extern "C" fn(
    result: zlink_request_result_t,
    parts: *mut zlink_msg_t,
    part_count: usize,
    userdata: *mut c_void,
);

pub type zlink_monitor_handler_fn =
    unsafe extern "C" fn(event: *const zlink_monitor_event_t, userdata: *mut c_void);
pub type zlink_socket_monitor_handler_fn = zlink_monitor_handler_fn;

pub type zlink_timer_handler_fn =
    unsafe extern "C" fn(timer: *mut c_void, fire_count: u64, userdata: *mut c_void);

// ---------------------------------------------------------------------------
// Functions exported by the candidate Core 11 headers
// ---------------------------------------------------------------------------

unsafe extern "C" {
    pub fn zlink_errno() -> c_int;
    pub fn zlink_strerror(errnum: c_int) -> *const c_char;
    pub fn zlink_version(major: *mut c_int, minor: *mut c_int, patch: *mut c_int);

    pub fn zlink_ctx_new() -> *mut c_void;
    pub fn zlink_ctx_term(ctx: *mut c_void) -> c_int;
    pub fn zlink_ctx_shutdown(ctx: *mut c_void) -> c_int;
    pub fn zlink_ctx_set(ctx: *mut c_void, option: zlink_ctx_option_t, optval: c_int) -> c_int;
    pub fn zlink_ctx_set_data(
        ctx: *mut c_void,
        option: zlink_ctx_option_t,
        optval: *const c_void,
        optvallen: usize,
    ) -> c_int;
    pub fn zlink_ctx_get_data(
        ctx: *mut c_void,
        option: zlink_ctx_option_t,
        optval: *mut c_void,
        optvallen: *mut usize,
    ) -> c_int;
    pub fn zlink_ctx_get(
        ctx: *mut c_void,
        option: zlink_ctx_option_t,
        error_out: *mut zlink_config_result_t,
    ) -> c_int;
    pub fn zlink_ctx_auto_hwm_recalculate(ctx: *mut c_void) -> c_int;

    pub fn zlink_msg_init(msg: *mut zlink_msg_t) -> c_int;
    pub fn zlink_msg_init_size(msg: *mut zlink_msg_t, size: usize) -> c_int;
    pub fn zlink_msg_close(msg: *mut zlink_msg_t) -> c_int;
    pub fn zlink_msg_move(dest: *mut zlink_msg_t, src: *mut zlink_msg_t) -> c_int;
    pub fn zlink_msg_copy(dest: *mut zlink_msg_t, src: *mut zlink_msg_t) -> c_int;
    pub fn zlink_msg_data(msg: *mut zlink_msg_t) -> *mut c_void;
    pub fn zlink_msg_size(msg: *const zlink_msg_t) -> usize;
    pub fn zlink_msg_refcnt(
        msg: *const zlink_msg_t,
        error_out: *mut zlink_config_result_t,
    ) -> c_int;
    pub fn zlink_multipart_close(parts: *mut zlink_msg_t, part_count: usize);

    pub fn zlink_socket(ctx: *mut c_void, typ: zlink_socket_type_t) -> *mut c_void;
    pub fn zlink_stream_packet_handler(
        stream: *mut c_void,
        handler: zlink_stream_packet_handler_fn,
        userdata: *mut c_void,
    ) -> c_int;
    pub fn zlink_send_ready_handler(
        socket: *mut c_void,
        handler: zlink_send_ready_handler_fn,
        userdata: *mut c_void,
    ) -> c_int;
    pub fn zlink_close(socket: *mut c_void) -> c_int;

    pub fn zlink_set_option(
        handle: *mut c_void,
        option: zlink_option_t,
        optval: *const c_void,
        optvallen: usize,
    ) -> c_int;
    pub fn zlink_get_option(
        handle: *mut c_void,
        option: zlink_option_t,
        optval: *mut c_void,
        optvallen: *mut usize,
    ) -> c_int;
    pub fn zlink_set_routing_id(handle: *mut c_void, data: *const c_void, size: usize) -> c_int;
    pub fn zlink_get_routing_id(handle: *mut c_void, out: *mut zlink_routing_id_t) -> c_int;
    pub fn zlink_set_tls_server(
        handle: *mut c_void,
        cert: *const c_char,
        key: *const c_char,
        require_client_cert: c_int,
    ) -> c_int;
    pub fn zlink_set_tls_client(
        handle: *mut c_void,
        ca_cert: *const c_char,
        hostname: *const c_char,
        trust_system: c_int,
    ) -> c_int;
    pub fn zlink_set_router_option(
        handle: *mut c_void,
        option: zlink_router_option_t,
        optval: *const c_void,
        optvallen: usize,
    ) -> c_int;
    pub fn zlink_get_router_option(
        handle: *mut c_void,
        option: zlink_router_option_t,
        optval: *mut c_void,
        optvallen: *mut usize,
    ) -> c_int;
    pub fn zlink_set_dealer_option(
        handle: *mut c_void,
        option: zlink_dealer_option_t,
        optval: *const c_void,
        optvallen: usize,
    ) -> c_int;
    pub fn zlink_get_dealer_option(
        handle: *mut c_void,
        option: zlink_dealer_option_t,
        optval: *mut c_void,
        optvallen: *mut usize,
    ) -> c_int;
    pub fn zlink_set_pub_option(
        handle: *mut c_void,
        option: zlink_pub_option_t,
        optval: *const c_void,
        optvallen: usize,
    ) -> c_int;
    pub fn zlink_get_pub_option(
        handle: *mut c_void,
        option: zlink_pub_option_t,
        optval: *mut c_void,
        optvallen: *mut usize,
    ) -> c_int;
    pub fn zlink_get_sub_option(
        handle: *mut c_void,
        option: zlink_sub_option_t,
        optval: *mut c_void,
        optvallen: *mut usize,
    ) -> c_int;
    pub fn zlink_set_stream_option(
        handle: *mut c_void,
        option: zlink_stream_option_t,
        optval: *const c_void,
        optvallen: usize,
    ) -> c_int;
    pub fn zlink_get_stream_option(
        handle: *mut c_void,
        option: zlink_stream_option_t,
        optval: *mut c_void,
        optvallen: *mut usize,
    ) -> c_int;

    pub fn zlink_bind(socket: *mut c_void, addr: *const c_char) -> c_int;
    pub fn zlink_connect(socket: *mut c_void, addr: *const c_char) -> c_int;
    pub fn zlink_unbind(socket: *mut c_void, addr: *const c_char) -> c_int;
    pub fn zlink_disconnect(socket: *mut c_void, addr: *const c_char) -> c_int;
    pub fn zlink_disconnect_rid(socket: *mut c_void, peer_rid: *const zlink_routing_id_t) -> c_int;

    pub fn zlink_send_part(
        socket: *mut c_void,
        part: *mut zlink_msg_t,
        flags: zlink_send_flags_t,
        part_flag: zlink_part_flag_t,
    ) -> c_int;
    pub fn zlink_send_part_rid(
        socket: *mut c_void,
        target_rid: *const zlink_routing_id_t,
        part: *mut zlink_msg_t,
        flags: zlink_send_flags_t,
        part_flag: zlink_part_flag_t,
    ) -> c_int;
    pub fn zlink_dealer_request_part(
        dealer: *mut c_void,
        part: *mut zlink_msg_t,
        flags: zlink_send_flags_t,
        part_flag: zlink_part_flag_t,
        timeout_ms: u32,
        handler: Option<zlink_reply_handler_fn>,
        userdata: *mut c_void,
    ) -> c_int;
    pub fn zlink_router_request_part(
        router: *mut c_void,
        peer_rid: *const zlink_routing_id_t,
        part: *mut zlink_msg_t,
        flags: zlink_send_flags_t,
        part_flag: zlink_part_flag_t,
        timeout_ms: u32,
        handler: Option<zlink_reply_handler_fn>,
        userdata: *mut c_void,
    ) -> c_int;
    pub fn zlink_router_reply_part(
        router: *mut c_void,
        peer_rid: *const zlink_routing_id_t,
        request_seq: u64,
        part: *mut zlink_msg_t,
        part_flag: zlink_part_flag_t,
    ) -> c_int;
    pub fn zlink_router_recv_part(
        router: *mut c_void,
        source_rid_out: *mut *const zlink_routing_id_t,
        request_seq_out: *mut u64,
        part_out: *mut zlink_msg_t,
        has_more_out: *mut zlink_part_flag_t,
        flags: zlink_recv_flags_t,
    ) -> c_int;
    pub fn zlink_recv_part(
        socket: *mut c_void,
        source_rid_out: *mut *const zlink_routing_id_t,
        part_out: *mut zlink_msg_t,
        has_more_out: *mut zlink_part_flag_t,
        flags: zlink_recv_flags_t,
    ) -> c_int;

    pub fn zlink_publish_part(
        subject: *mut c_void,
        topic_id: *const c_char,
        part: *mut zlink_msg_t,
        flags: zlink_send_flags_t,
        part_flag: zlink_part_flag_t,
    ) -> c_int;
    pub fn zlink_set_subscription(handle: *mut c_void, filter: *const c_char) -> c_int;
    pub fn zlink_unset_subscription(handle: *mut c_void, filter: *const c_char) -> c_int;
    pub fn zlink_subscription_at(
        handle: *mut c_void,
        index: usize,
        filter_out: *mut c_char,
        filter_len_inout: *mut usize,
        is_pattern_out: *mut c_int,
    ) -> c_int;
    pub fn zlink_subscribe_part(
        subject: *mut c_void,
        source_rid_out: *mut *const zlink_routing_id_t,
        topic_id_out: *mut c_char,
        topic_id_capacity: usize,
        topic_id_len_out: *mut usize,
        part_out: *mut zlink_msg_t,
        has_more_out: *mut zlink_part_flag_t,
        flags: zlink_recv_flags_t,
    ) -> c_int;
    pub fn zlink_xpub_recv_part(
        subject: *mut c_void,
        source_rid_out: *mut *const zlink_routing_id_t,
        subscribed_out: *mut c_int,
        topic_id_out: *mut c_char,
        topic_id_capacity: usize,
        topic_id_len_out: *mut usize,
        flags: zlink_recv_flags_t,
    ) -> c_int;

    pub fn zlink_socket_monitor_open(
        socket: *mut c_void,
        options: *const zlink_socket_monitor_open_options_t,
    ) -> *mut c_void;
    pub fn zlink_socket_monitor_handler(
        monitor: *mut c_void,
        handler: zlink_socket_monitor_handler_fn,
        userdata: *mut c_void,
    ) -> c_int;
    pub fn zlink_socket_monitor_recv(
        monitor: *mut c_void,
        out: *mut zlink_socket_monitor_event_t,
        flags: zlink_recv_flags_t,
    ) -> c_int;
    pub fn zlink_monitor_status(monitor: *mut c_void, out: *mut zlink_monitor_status_t) -> c_int;
    pub fn zlink_monitor_close(monitor_p: *mut *mut c_void) -> c_int;

    pub fn zlink_poll(
        items: *mut zlink_pollitem_t,
        nitems: c_int,
        timeout: c_long,
        error_out: *mut c_int,
    ) -> c_int;
    pub fn zlink_poller_new() -> *mut c_void;
    pub fn zlink_poller_destroy(poller_p: *mut *mut c_void) -> c_int;
    pub fn zlink_poller_size(poller: *mut c_void, error_out: *mut c_int) -> c_int;
    pub fn zlink_poller_add(
        poller: *mut c_void,
        socket: *mut c_void,
        user_data: *mut c_void,
        events: i16,
    ) -> c_int;
    pub fn zlink_poller_modify(poller: *mut c_void, socket: *mut c_void, events: i16) -> c_int;
    pub fn zlink_poller_remove(poller: *mut c_void, socket: *mut c_void) -> c_int;
    pub fn zlink_poller_add_fd(
        poller: *mut c_void,
        fd: zlink_fd_t,
        user_data: *mut c_void,
        events: i16,
    ) -> c_int;
    pub fn zlink_poller_modify_fd(poller: *mut c_void, fd: zlink_fd_t, events: i16) -> c_int;
    pub fn zlink_poller_remove_fd(poller: *mut c_void, fd: zlink_fd_t) -> c_int;
    pub fn zlink_poller_add_timer(
        poller: *mut c_void,
        timer: *mut c_void,
        user_data: *mut c_void,
    ) -> c_int;
    pub fn zlink_poller_remove_timer(poller: *mut c_void, timer: *mut c_void) -> c_int;
    pub fn zlink_poller_wait(
        poller: *mut c_void,
        events: *mut zlink_poller_event_t,
        n_events: c_int,
        timeout: c_long,
        error_out: *mut c_int,
    ) -> c_int;

    pub fn zlink_timer_new() -> *mut c_void;
    pub fn zlink_timer_destroy(timer_p: *mut *mut c_void) -> c_int;
    pub fn zlink_timer_start(timer: *mut c_void, interval_ns: u64, repeat_count: u64) -> c_int;
    pub fn zlink_timer_stop(timer: *mut c_void) -> c_int;
    pub fn zlink_timer_recv(timer: *mut c_void, fire_count_out: *mut u64) -> c_int;
    pub fn zlink_timer_handler(
        timer: *mut c_void,
        handler: zlink_timer_handler_fn,
        userdata: *mut c_void,
    ) -> c_int;

    pub fn zlink_stopwatch_start() -> *mut c_void;
    pub fn zlink_stopwatch_intermediate(watch: *mut c_void) -> c_ulong;
    pub fn zlink_stopwatch_stop(watch: *mut c_void) -> c_ulong;
    pub fn zlink_atomic_counter_new() -> *mut c_void;
    pub fn zlink_atomic_counter_set(counter: *mut c_void, value: c_int);
    pub fn zlink_atomic_counter_inc(counter: *mut c_void) -> c_int;
    pub fn zlink_atomic_counter_dec(counter: *mut c_void) -> c_int;
    pub fn zlink_atomic_counter_value(counter: *mut c_void) -> c_int;
    pub fn zlink_atomic_counter_destroy(counter_p: *mut *mut c_void);
    pub fn zlink_thread_start(
        func: Option<unsafe extern "C" fn(*mut c_void)>,
        arg: *mut c_void,
    ) -> *mut c_void;
    pub fn zlink_thread_join(thread: *mut c_void);

    pub fn zlink_proxy(frontend: *mut c_void, backend: *mut c_void, capture: *mut c_void) -> c_int;
    pub fn zlink_proxy_steerable(
        frontend: *mut c_void,
        backend: *mut c_void,
        capture: *mut c_void,
        control: *mut c_void,
    ) -> c_int;
    pub fn zlink_has(capability: *const c_char) -> c_int;
    pub fn zlink_sleep(seconds: c_int);
}

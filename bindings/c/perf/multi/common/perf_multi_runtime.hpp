#ifndef PERF_MULTI_RUNTIME_HPP
#define PERF_MULTI_RUNTIME_HPP

#include "../../common/perf_infra.hpp"
#include "perf_common_multi.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <vector>

static const std::vector<size_t> MSG_SIZES = {64, 256, 1024, 65536, 131072, 262144};
static const std::vector<std::string> TRANSPORTS = {"tcp", "inproc", "ipc"};
static const std::vector<std::string> STREAM_TRANSPORTS = {"tcp", "tls", "ws", "wss"};

inline const char *resolve_multi_named_env_value (const char *name_)
{
    if (!name_ || !*name_)
        return NULL;

    if (std::strcmp (name_, "PERF_CLIENTS") == 0)
        return resolve_multi_env_value ("PERF_MULTI_CLIENTS", "PERF_CLIENTS");
    if (std::strcmp (name_, "PERF_SNDHWM") == 0)
        return resolve_multi_env_value ("PERF_MULTI_SNDHWM", "PERF_SNDHWM");
    if (std::strcmp (name_, "PERF_RCVHWM") == 0)
        return resolve_multi_env_value ("PERF_MULTI_RCVHWM", "PERF_RCVHWM");
    if (std::strcmp (name_, "PERF_SNDTIMEO_MS") == 0)
        return resolve_multi_env_value ("PERF_MULTI_SNDTIMEO_MS", "PERF_SNDTIMEO_MS");
    if (std::strcmp (name_, "PERF_RCVTIMEO_MS") == 0)
        return resolve_multi_env_value ("PERF_MULTI_RCVTIMEO_MS", "PERF_RCVTIMEO_MS");
    if (std::strcmp (name_, "PERF_SNDBUF") == 0)
        return resolve_multi_env_value ("PERF_MULTI_SNDBUF", "PERF_SNDBUF");
    if (std::strcmp (name_, "PERF_RCVBUF") == 0)
        return resolve_multi_env_value ("PERF_MULTI_RCVBUF", "PERF_RCVBUF");
    if (std::strcmp (name_, "PERF_MONITOR_HWM") == 0)
        return resolve_multi_env_value ("PERF_MULTI_MONITOR_HWM", "PERF_MONITOR_HWM");

    return resolve_multi_env_value (name_, NULL);
}

inline int bench_io_threads ()
{
    return parse_positive_env ("PERF_IO_THREADS", 4);
}

inline int bench_max_sockets ()
{
    const int explicit_max = parse_positive_env ("PERF_MAX_SOCKETS", 0);
    if (explicit_max > 0)
        return explicit_max;

    const int clients =
      resolve_multi_int_env_with_fallback ("PERF_MULTI_CLIENTS", "PERF_CLIENTS", 0, 0);
    if (clients <= 0)
        return 0;

    const long required = static_cast<long> (clients) * 3L + 4096L;
    if (required > INT_MAX)
        return INT_MAX;
    return static_cast<int> (required);
}

inline int bench_ctx_blocky ()
{
    const char *value = std::getenv ("PERF_CTX_BLOCKY");
    if (!value || !*value)
        return 0;

    errno = 0;
    char *end = NULL;
    const long parsed = std::strtol (value, &end, 10);
    if (errno != 0 || end == value)
        return 0;
    return parsed != 0 ? 1 : 0;
}

inline int bench_ctx_auto_hwm_enable ()
{
    const char *value = std::getenv ("PERF_CTX_AUTO_HWM_ENABLE");
    if (!value || !*value)
        return ZLINK_CTX_AUTO_HWM_ENABLE_DFLT;

    errno = 0;
    char *end = NULL;
    const long parsed = std::strtol (value, &end, 10);
    if (errno != 0 || end == value)
        return ZLINK_CTX_AUTO_HWM_ENABLE_DFLT;
    return parsed != 0 ? 1 : 0;
}

inline int bench_ctx_auto_hwm_profile ()
{
    const char *value = std::getenv ("PERF_CTX_AUTO_HWM_PROFILE");
    if (!value || !*value)
        return ZLINK_CTX_AUTO_HWM_PROFILE_DFLT;

    if (std::strcmp (value, "compact") == 0)
        return ZLINK_AUTO_HWM_PROFILE_COMPACT;
    if (std::strcmp (value, "low_latency") == 0 || std::strcmp (value, "low-latency") == 0)
        return ZLINK_AUTO_HWM_PROFILE_LOW_LATENCY;
    if (std::strcmp (value, "balanced") == 0)
        return ZLINK_AUTO_HWM_PROFILE_BALANCED;
    if (std::strcmp (value, "throughput") == 0)
        return ZLINK_AUTO_HWM_PROFILE_THROUGHPUT;

    return ZLINK_CTX_AUTO_HWM_PROFILE_DFLT;
}

inline bool perf_auto_hwm_detail_enabled ()
{
    const char *value =
      resolve_multi_env_value ("PERF_MULTI_PRINT_AUTO_HWM_DETAIL", "PERF_PRINT_AUTO_HWM_DETAIL");
    if (!value || !*value)
        return true;
    return std::strcmp (value, "0") != 0;
}

inline const char *perf_auto_hwm_role_name (uint32_t role_)
{
    switch (role_) {
        case 1:
            return "control";
        case 2:
            return "routed";
        case 3:
            return "fanout";
        case 4:
            return "recv_ingress";
        case 6:
            return "peer_queue";
        case 7:
            return "stream";
        default:
            return "none";
    }
}

inline const char *perf_auto_hwm_profile_name (uint32_t profile_)
{
    switch (profile_) {
        case ZLINK_AUTO_HWM_PROFILE_COMPACT:
            return "compact";
        case ZLINK_AUTO_HWM_PROFILE_LOW_LATENCY:
            return "low_latency";
        case ZLINK_AUTO_HWM_PROFILE_BALANCED:
            return "balanced";
        case ZLINK_AUTO_HWM_PROFILE_THROUGHPUT:
            return "throughput";
        default:
            return "unknown";
    }
}

inline const char *perf_auto_hwm_policy_class_name (uint32_t policy_class_)
{
    switch (policy_class_) {
        case 1:
            return "fanout";
        case 3:
            return "recv_ingress";
        case 4:
            return "routed";
        case 5:
            return "peer_queue";
        case 6:
            return "stream";
        case 7:
            return "control";
        default:
            return "none";
    }
}

inline const char *perf_auto_hwm_recalc_reason_name (uint32_t reason_)
{
    switch (reason_) {
        case ZLINK_AUTO_HWM_RECALC_REASON_INITIAL:
            return "initial";
        case ZLINK_AUTO_HWM_RECALC_REASON_ROLE_CHANGE:
            return "role_change";
        case ZLINK_AUTO_HWM_RECALC_REASON_POLICY_TOGGLE:
            return "policy_toggle";
        case ZLINK_AUTO_HWM_RECALC_REASON_REFRESH:
            return "refresh";
        case ZLINK_AUTO_HWM_RECALC_REASON_DEFERRED_SHRINK:
            return "deferred_shrink";
        default:
            return "none";
    }
}

inline std::string perf_auto_hwm_env_or_default (const char *name_, const char *fallback_)
{
    const char *value = resolve_multi_env_value (name_, NULL);
    if (value && *value)
        return std::string (value);
    return fallback_ ? std::string (fallback_) : std::string ();
}

inline const char *perf_socket_type_name (uint32_t type_)
{
    switch (type_) {
        case ZLINK_SOCKET_PAIR:
            return "pair";
        case ZLINK_SOCKET_PUB:
            return "pub";
        case ZLINK_SOCKET_SUB:
            return "sub";
        case ZLINK_SOCKET_DEALER:
            return "dealer";
        case ZLINK_SOCKET_ROUTER:
            return "router";
        case ZLINK_SOCKET_XPUB:
            return "xpub";
        case ZLINK_SOCKET_XSUB:
            return "xsub";
        case ZLINK_SOCKET_STREAM:
            return "stream";
        default:
            return "unknown";
    }
}

inline bool perf_auto_hwm_send_side_visible (uint32_t socket_type_, uint32_t role_)
{
    const char *role_name = perf_auto_hwm_role_name (role_);
    if ((socket_type_ == ZLINK_SOCKET_SUB || socket_type_ == ZLINK_SOCKET_XSUB)
        && (std::strcmp (role_name, "recv_ingress") == 0
            || std::strcmp (role_name, "control") == 0))
        return false;
    return true;
}

inline bool perf_auto_hwm_recv_side_visible (uint32_t socket_type_, uint32_t role_)
{
    const char *role_name = perf_auto_hwm_role_name (role_);
    if ((socket_type_ == ZLINK_SOCKET_PUB || socket_type_ == ZLINK_SOCKET_XPUB)
        && std::strcmp (role_name, "control") == 0)
        return false;
    return true;
}

inline const char *perf_auto_hwm_bucket_label (uint32_t bucket_index_)
{
    switch (bucket_index_) {
    case 0:
        return "1-64";
    case 1:
        return "65-128";
    case 2:
        return "129-512";
    case 3:
        return "513-2048";
    case 4:
        return "2049+";
    default:
        return "";
    }
}

inline std::string perf_auto_hwm_sndhwm_display (const zlink_monitor_status_t &snapshot_,
                                                 uint32_t socket_type_)
{
    if (!perf_auto_hwm_send_side_visible (socket_type_, snapshot_.auto_hwm_role))
        return "-";
    return std::to_string (snapshot_.auto_hwm_applied_sndhwm_bytes);
}

inline std::string perf_auto_hwm_rcvhwm_display (const zlink_monitor_status_t &snapshot_,
                                                 uint32_t socket_type_)
{
    if (!perf_auto_hwm_recv_side_visible (socket_type_, snapshot_.auto_hwm_role))
        return "-";
    return std::to_string (snapshot_.auto_hwm_applied_rcvhwm_bytes);
}

inline std::string perf_auto_hwm_sndbuf_display (const zlink_monitor_status_t &snapshot_,
                                                 uint32_t socket_type_)
{
    if (!perf_auto_hwm_send_side_visible (socket_type_, snapshot_.auto_hwm_role))
        return "0";
    return std::to_string (snapshot_.auto_hwm_effective_sndbuf);
}

inline std::string perf_auto_hwm_rcvbuf_display (const zlink_monitor_status_t &snapshot_,
                                                 uint32_t socket_type_)
{
    if (!perf_auto_hwm_recv_side_visible (socket_type_, snapshot_.auto_hwm_role))
        return "0";
    return std::to_string (snapshot_.auto_hwm_effective_rcvbuf);
}

inline size_t perf_auto_hwm_effective_msg_size (size_t msg_size_)
{
    if (msg_size_ != 0)
        return msg_size_;
    const char *value = std::getenv ("PERF_MSG_SIZES");
    if (!value || !*value)
        value = std::getenv ("PERF_MULTI_MSG_SIZES");
    if (!value || !*value)
        return 0;
    errno = 0;
    char *end = NULL;
    const unsigned long parsed = std::strtoul (value, &end, 10);
    if (errno != 0 || end == value)
        return 0;
    return static_cast<size_t> (parsed);
}

inline bool perf_auto_hwm_label_is_control_snapshot (const char *label_)
{
    return label_ && std::strstr (label_, "control") != NULL;
}

inline void perf_print_auto_hwm_snapshot (void *handle_,
                                          bool service_handle_,
                                          const char *label_,
                                          const std::string &transport_,
                                          bool allow_service_fallback_ = true,
                                          size_t msg_size_ = 0,
                                          zlink_socket_type_t socket_type_ = ZLINK_SOCKET_ANY)
{
    if (!handle_ || !perf_auto_hwm_detail_enabled ())
        return;

    zlink_monitor_status_t snapshot;
    std::memset (&snapshot, 0, sizeof (snapshot));

    void *monitor = NULL;
    zlink_config_result_t rc = ZLINK_CONFIG_INVALID_HANDLE;
    if (service_handle_) {
        rc = ZLINK_CONFIG_INVALID_HANDLE;
    } else {
        zlink_socket_monitor_open_options_t opts;
        std::memset (&opts, 0, sizeof (opts));
        opts.events = ZLINK_SOCKET_MONITOR_EVENT_ALL;
        monitor = zlink_socket_monitor_open (handle_, &opts);
        if (monitor) {
            rc = zlink_monitor_status (monitor, &snapshot);
            zlink_monitor_close (&monitor);
        }
    }
    const bool snapshot_from_monitor = rc == ZLINK_CONFIG_OK;
    if (!snapshot_from_monitor) {
        uint64_t hwm_value = 0;
        size_t hwm_value_size = sizeof (hwm_value);
        std::memset (&snapshot, 0, sizeof (snapshot));
        if (zlink_get_option (
              handle_, ZLINK_OPT_SNDHWM, &hwm_value, &hwm_value_size)
            == ZLINK_CONFIG_OK)
            snapshot.auto_hwm_applied_sndhwm_bytes = hwm_value;
        hwm_value = 0;
        hwm_value_size = sizeof (hwm_value);
        if (zlink_get_option (
              handle_, ZLINK_OPT_RCVHWM, &hwm_value, &hwm_value_size)
            == ZLINK_CONFIG_OK)
            snapshot.auto_hwm_applied_rcvhwm_bytes = hwm_value;
        int value = 0;
        size_t value_size = sizeof (value);
        if (zlink_get_option (handle_, ZLINK_OPT_SNDBUF, &value, &value_size) == ZLINK_CONFIG_OK)
            snapshot.auto_hwm_effective_sndbuf = value;
        value = 0;
        value_size = sizeof (value);
        if (zlink_get_option (handle_, ZLINK_OPT_RCVBUF, &value, &value_size) == ZLINK_CONFIG_OK)
            snapshot.auto_hwm_effective_rcvbuf = value;
    }

    static std::mutex sync;
    static std::set<std::string> emitted;
    const std::string pattern = perf_auto_hwm_env_or_default ("PERF_MULTI_PATTERN", "unknown");
    const std::string component = perf_auto_hwm_env_or_default ("PERF_MULTI_COMPONENT", "process");
    const std::string transport =
      transport_.empty () ? perf_auto_hwm_env_or_default ("PERF_MULTI_TRANSPORT", "unknown")
                          : transport_;
    const char *label = label_ && *label_ ? label_ : "socket";

    const std::string key = pattern + "|" + transport + "|" + component + "|" + label + "|"
                            + std::to_string (msg_size_) + "|"
                            + perf_auto_hwm_role_name (snapshot.auto_hwm_role) + "|"
                            + std::to_string (snapshot.auto_hwm_applied_sndhwm_bytes) + "|"
                            + std::to_string (snapshot.auto_hwm_applied_rcvhwm_bytes) + "|"
                            + std::to_string (snapshot.auto_hwm_profile) + "|"
                            + std::to_string (snapshot.auto_hwm_policy_class) + "|"
                            + std::to_string (snapshot.auto_hwm_unit_budget_bytes) + "|"
                            + std::to_string (snapshot.auto_hwm_size_cap) + "|"
                            + std::to_string (snapshot.auto_hwm_socket_message_slots) + "|"
                            + std::to_string (snapshot.auto_hwm_effective_message_bytes) + "|"
                            + std::to_string (snapshot.auto_hwm_effective_sndbuf) + "|"
                            + std::to_string (snapshot.auto_hwm_effective_rcvbuf);

    {
        std::lock_guard<std::mutex> lock (sync);
        if (emitted.find (key) != emitted.end ())
            return;
        emitted.insert (key);
    }

    std::cout << "AUTO_HWM_DETAIL" << ",pattern=" << pattern << ",transport=" << transport
              << ",component=" << component << ",label=" << label
              << ",socket_type=" << perf_socket_type_name (socket_type_)
              << ",msg_size=" << msg_size_
              << ",source=" << (snapshot_from_monitor ? "monitor_snapshot" : "option_fallback")
              << ",enabled=" << snapshot.auto_hwm_enabled
              << ",role=" << perf_auto_hwm_role_name (snapshot.auto_hwm_role)
              << ",role_id=" << snapshot.auto_hwm_role
              << ",profile=" << perf_auto_hwm_profile_name (snapshot.auto_hwm_profile)
              << ",profile_id=" << snapshot.auto_hwm_profile << ",policy_class="
              << perf_auto_hwm_policy_class_name (snapshot.auto_hwm_policy_class)
              << ",policy_class_id=" << snapshot.auto_hwm_policy_class
              << ",unit_budget_bytes=" << snapshot.auto_hwm_unit_budget_bytes
              << ",size_cap=" << snapshot.auto_hwm_size_cap
              << ",sndhwm=" << snapshot.auto_hwm_applied_sndhwm_bytes
              << ",rcvhwm=" << snapshot.auto_hwm_applied_rcvhwm_bytes
              << ",socket_message_slots=" << snapshot.auto_hwm_socket_message_slots
              << ",effective_message_bytes=" << snapshot.auto_hwm_effective_message_bytes
              << ",effective_sndbuf=" << perf_auto_hwm_sndbuf_display (snapshot, socket_type_)
              << ",effective_rcvbuf=" << perf_auto_hwm_rcvbuf_display (snapshot, socket_type_)
              << ",last_recalc_ms=" << snapshot.auto_hwm_last_recalc_ms << ",last_recalc_reason="
              << perf_auto_hwm_recalc_reason_name (snapshot.auto_hwm_last_recalc_reason)
              << ",send_blocked_ratio_ppm=" << snapshot.auto_hwm_send_blocked_ratio_ppm
              << ",deferred_sndhwm=" << snapshot.auto_hwm_deferred_sndhwm_bytes
              << ",deferred_rcvhwm=" << snapshot.auto_hwm_deferred_rcvhwm_bytes
              << ",deferred_sndhwm_valid=" << snapshot.auto_hwm_deferred_sndhwm_valid
              << ",deferred_rcvhwm_valid=" << snapshot.auto_hwm_deferred_rcvhwm_valid
              << std::endl;
}

inline void apply_ctx_options (void *ctx_)
{
    const bool debug = std::getenv ("PERF_DEBUG") != NULL;
    const int io_threads = bench_io_threads ();
    if (io_threads > 0) {
        const int rc = zlink_ctx_set (ctx_, ZLINK_IO_THREADS, io_threads);
        if (rc != 0 && debug) {
            std::cerr << "zlink_ctx_set(ZLINK_IO_THREADS) failed: "
                      << zlink_strerror (zlink_errno ()) << std::endl;
        }
    }

    const int max_sockets = bench_max_sockets ();
    if (max_sockets > 0) {
        const int rc = zlink_ctx_set (ctx_, ZLINK_MAX_SOCKETS, max_sockets);
        if (rc != 0 && debug) {
            std::cerr << "zlink_ctx_set(ZLINK_MAX_SOCKETS) failed: "
                      << zlink_strerror (zlink_errno ()) << std::endl;
        }
    }

    const int blocky = bench_ctx_blocky ();
    set_ctx_opt_int (ctx_, ZLINK_CTX_OPT_BLOCKY, blocky, "ZLINK_CTX_OPT_BLOCKY");
    set_ctx_opt_int (ctx_, ZLINK_CTX_OPT_AUTO_HWM_ENABLE, bench_ctx_auto_hwm_enable (),
                     "ZLINK_CTX_OPT_AUTO_HWM_ENABLE");
    set_ctx_opt_int (ctx_, ZLINK_CTX_OPT_AUTO_HWM_PROFILE, bench_ctx_auto_hwm_profile (),
                     "ZLINK_CTX_OPT_AUTO_HWM_PROFILE");
}

class ctx_guard_t
{
  public:
    ctx_guard_t () : _ctx (zlink_ctx_new ())
    {
        if (_ctx)
            apply_ctx_options (_ctx);
    }
    ~ctx_guard_t ()
    {
        if (_ctx) {
            zlink_ctx_shutdown (_ctx);

            const char *term_env = std::getenv ("PERF_CTX_TERM");
            if (term_env && std::strcmp (term_env, "0") != 0)
                zlink_ctx_term (_ctx);
        }
    }

    void force_term ()
    {
        if (!_ctx)
            return;
        zlink_ctx_shutdown (_ctx);
        zlink_ctx_term (_ctx);
        _ctx = NULL;
    }

    void *get () const { return _ctx; }
    bool valid () const { return _ctx != NULL; }

  private:
    ctx_guard_t (const ctx_guard_t &);
    ctx_guard_t &operator= (const ctx_guard_t &);

    void *_ctx;
};

class socket_guard_t
{
  public:
    socket_guard_t () : _socket (NULL) {}
    socket_guard_t (void *ctx_, zlink_socket_type_t type_) : _socket (zlink_socket (ctx_, type_)) {}
    ~socket_guard_t ()
    {
        if (_socket)
            zlink_close (_socket);
    }

    void *get () const { return _socket; }
    bool valid () const { return _socket != NULL; }
    operator void * () const { return _socket; }

  private:
    socket_guard_t (const socket_guard_t &);
    socket_guard_t &operator= (const socket_guard_t &);

    void *_socket;
};

inline int
zlink_stream_send_msg (void *socket_, const zlink_routing_id_t *rid_, zlink_msg_t *msg_, int flags_)
{
    const size_t size = zlink_msg_size (msg_);
    return ::perf_zlink_send_rid_parts (socket_, rid_, msg_, 1,
                                        static_cast<zlink_send_flags_t> (flags_))
               == 0
             ? static_cast<int> (size)
             : -1;
}

inline int perf_zlink_subscribe_parts (void *sub_,
                                       zlink_msg_t **parts_,
                                       size_t *part_count_,
                                       int flags_,
                                       char *topic_id_out_,
                                       size_t *topic_id_len_)
{
    return ::perf_zlink_subscribe_parts (sub_, NULL, parts_, part_count_, topic_id_out_,
                                         topic_id_len_, static_cast<zlink_recv_flags_t> (flags_));
}

inline bool bench_transition_debug_enabled ()
{
    static const bool enabled = std::getenv ("PERF_DEBUG_TRANSITIONS") != nullptr;
    return enabled;
}

inline bool bench_manual_socket_overrides_allowed ()
{
    const char *value = resolve_multi_env_value ("PERF_MULTI_ALLOW_MANUAL_SOCKET_OVERRIDES",
                                                 "PERF_ALLOW_MANUAL_SOCKET_OVERRIDES");
    return value && std::strcmp (value, "1") == 0;
}

inline uint64_t bench_hwm_from_env (const char *name_, uint64_t default_hwm_)
{
    if (!name_ || !*name_)
        return default_hwm_;

    const char *value = resolve_multi_named_env_value (name_);
    if (!value || !*value)
        return default_hwm_;
    if (std::strspn (value, "0123456789") != std::strlen (value))
        return default_hwm_;

    errno = 0;
    char *end = NULL;
    const unsigned long long parsed = std::strtoull (value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0)
        return default_hwm_;
    return static_cast<uint64_t> (parsed);
}

inline void apply_benchmark_hwm (void *socket_, uint64_t hwm_value)
{
    if (!bench_manual_socket_overrides_allowed ())
        return;

    const char *sndhwm_raw = resolve_multi_named_env_value ("PERF_SNDHWM");
    const char *rcvhwm_raw = resolve_multi_named_env_value ("PERF_RCVHWM");
    const bool explicit_sndhwm = sndhwm_raw && *sndhwm_raw;
    const bool explicit_rcvhwm = rcvhwm_raw && *rcvhwm_raw;
    if (hwm_value == 0 && !explicit_sndhwm && !explicit_rcvhwm)
        return;

    if (explicit_sndhwm || hwm_value != 0) {
        const uint64_t sndhwm =
          bench_hwm_from_env ("PERF_SNDHWM", hwm_value);
        if (sndhwm > 0)
            set_sockopt_u64 (socket_, ZLINK_OPT_SNDHWM, sndhwm, "ZLINK_OPT_SNDHWM");
    }
    if (explicit_rcvhwm || hwm_value != 0) {
        const uint64_t rcvhwm =
          bench_hwm_from_env ("PERF_RCVHWM", hwm_value);
        if (rcvhwm > 0)
            set_sockopt_u64 (socket_, ZLINK_OPT_RCVHWM, rcvhwm, "ZLINK_OPT_RCVHWM");
    }
}

inline uint64_t perf_auto_hwm_msg_unit_for_size (size_t msg_size_)
{
    return static_cast<uint64_t> (msg_size_);
}

inline bool apply_benchmark_context_auto_hwm_msg_unit (void *ctx_, size_t msg_size_)
{
    if (!ctx_ || msg_size_ == 0)
        return true;
    const uint64_t msg_unit = perf_auto_hwm_msg_unit_for_size (msg_size_);
    if (!set_ctx_opt_u64 (ctx_, ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES, msg_unit,
                          "ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES"))
        return false;
    return zlink_ctx_auto_hwm_recalculate (ctx_) == ZLINK_CONFIG_OK;
}

inline int bench_timeout_ms_from_env (const char *name_, int default_ms_)
{
    if (!name_ || !*name_)
        return default_ms_;

    const char *value = resolve_multi_named_env_value (name_);
    if (!value || !*value)
        return default_ms_;

    errno = 0;
    char *end = NULL;
    const long parsed = std::strtol (value, &end, 10);
    if (errno != 0 || end == value || parsed <= 0)
        return default_ms_;
    if (parsed > INT_MAX)
        return INT_MAX;
    return static_cast<int> (parsed);
}

inline int parse_byte_size_token (const char *value_, int default_value_)
{
    if (!value_ || !*value_)
        return default_value_;

    errno = 0;
    char *end = NULL;
    const unsigned long long parsed = std::strtoull (value_, &end, 10);
    if (errno != 0 || end == value_)
        return default_value_;

    unsigned long long multiplier = 1;
    if (end && *end) {
        char suffix[3] = {0, 0, 0};
        size_t suffix_len = 0;
        while (end[suffix_len] != '\0' && suffix_len < 2) {
            suffix[suffix_len] =
              static_cast<char> (std::tolower (static_cast<unsigned char> (end[suffix_len])));
            ++suffix_len;
        }
        if (end[suffix_len] != '\0')
            return default_value_;

        if (suffix[0] == 'b' && suffix[1] == '\0')
            multiplier = 1;
        else if (suffix[0] == 'k' && (suffix[1] == '\0' || suffix[1] == 'b'))
            multiplier = 1024ULL;
        else if (suffix[0] == 'm' && (suffix[1] == '\0' || suffix[1] == 'b'))
            multiplier = 1024ULL * 1024ULL;
        else if (suffix[0] == 'g' && (suffix[1] == '\0' || suffix[1] == 'b'))
            multiplier = 1024ULL * 1024ULL * 1024ULL;
        else
            return default_value_;
    }

    const unsigned long long bytes = parsed * multiplier;
    if (bytes == 0)
        return default_value_;
    if (bytes > static_cast<unsigned long long> (INT_MAX))
        return INT_MAX;
    return static_cast<int> (bytes);
}

inline int bench_socket_buffer_bytes_from_env (const char *name_, int default_bytes_)
{
    if (!name_ || !*name_)
        return default_bytes_;

    const char *value = resolve_multi_named_env_value (name_);
    if (!value || !*value)
        return default_bytes_;

    return parse_byte_size_token (value, default_bytes_);
}

inline void apply_debug_timeouts (void *socket_, const std::string &transport)
{
    if (transport == "inproc")
        return;

    const int sndtimeo_ms = bench_timeout_ms_from_env ("PERF_SNDTIMEO_MS", 200);
    const int rcvtimeo_ms = bench_timeout_ms_from_env ("PERF_RCVTIMEO_MS", 200);
    set_sockopt_int (socket_, ZLINK_OPT_SNDTIMEO, sndtimeo_ms, "ZLINK_OPT_SNDTIMEO");
    set_sockopt_int (socket_, ZLINK_OPT_RCVTIMEO, rcvtimeo_ms, "ZLINK_OPT_RCVTIMEO");
}

inline void apply_benchmark_socket_options (void *socket_,
                                            uint64_t hwm_value,
                                            const std::string &transport,
                                            zlink_socket_type_t socket_type = ZLINK_SOCKET_ANY,
                                            size_t msg_size = 0,
                                            bool print_auto_hwm_snapshot = true)
{
    if (!socket_)
        return;

    const int linger_ms = 0;
    const int tcp_nodelay = 1;
    const int sndbuf = bench_manual_socket_overrides_allowed ()
                         ? bench_socket_buffer_bytes_from_env ("PERF_SNDBUF", -1)
                         : -1;
    const int rcvbuf = bench_manual_socket_overrides_allowed ()
                         ? bench_socket_buffer_bytes_from_env ("PERF_RCVBUF", -1)
                         : -1;
    set_sockopt_int (socket_, ZLINK_OPT_LINGER, linger_ms, "ZLINK_OPT_LINGER");
    if (transport == "tcp") {
        set_sockopt_int (socket_, ZLINK_OPT_TCP_NODELAY, tcp_nodelay, "ZLINK_OPT_TCP_NODELAY");
    }
    if (sndbuf > 0)
        set_sockopt_int (socket_, ZLINK_OPT_SNDBUF, sndbuf, "ZLINK_OPT_SNDBUF");
    if (rcvbuf > 0)
        set_sockopt_int (socket_, ZLINK_OPT_RCVBUF, rcvbuf, "ZLINK_OPT_RCVBUF");
    apply_benchmark_hwm (socket_, hwm_value);
    apply_debug_timeouts (socket_, transport);
    if (print_auto_hwm_snapshot)
        perf_print_auto_hwm_snapshot (socket_, false, "endpoint", transport, false, msg_size,
                                      socket_type);
}

inline std::string transport_from_endpoint (const std::string &endpoint)
{
    const std::string::size_type pos = endpoint.find ("://");
    if (pos == std::string::npos)
        return std::string ();
    return endpoint.substr (0, pos);
}

inline std::string
bind_and_resolve_endpoint (void *socket_, const std::string &transport, const std::string &id)
{
    std::string endpoint = make_endpoint (transport, id);
    if (endpoint.empty ()) {
        std::cerr << "No endpoint available for transport " << transport << std::endl;
        return std::string ();
    }
    if (zlink_bind (socket_, endpoint.c_str ()) != ZLINK_BIND_OK) {
        std::cerr << "bind failed for " << endpoint << ": " << zlink_strerror (zlink_errno ())
                  << std::endl;
        return std::string ();
    }
    if (transport != "inproc") {
        char last_endpoint[MAX_SOCKET_STRING] = "";
        size_t size = sizeof (last_endpoint);
        if (zlink_get_option (socket_, ZLINK_OPT_LAST_ENDPOINT, last_endpoint, &size)
            != ZLINK_CONFIG_OK) {
            std::cerr << "getsockopt(ZLINK_LAST_ENDPOINT) failed: "
                      << zlink_strerror (zlink_errno ()) << std::endl;
            return std::string ();
        }
        endpoint.assign (last_endpoint);
        if (transport == "tcp" || transport == "ws") {
            const std::string tcp_any = "://0.0.0.0:";
            const std::string tcp_ipv6_any = "://[::]:";
            size_t pos = endpoint.find (tcp_any);
            if (pos != std::string::npos) {
                endpoint.replace (pos, tcp_any.size (), "://127.0.0.1:");
            } else {
                pos = endpoint.find (tcp_ipv6_any);
                if (pos != std::string::npos) {
                    endpoint.replace (pos, tcp_ipv6_any.size (), "://127.0.0.1:");
                }
            }
        }
        if (bench_debug_enabled ()) {
            std::cerr << "Resolved endpoint (" << transport << "): " << endpoint << std::endl;
        }
    }
    apply_debug_timeouts (socket_, transport);
    return endpoint;
}

inline bool transport_available (const std::string &transport)
{
    if (transport == "ipc")
        return zlink_has ("ipc") != 0;
    return true;
}

inline bool connect_checked (void *socket_,
                             const std::string &endpoint,
                             const std::string &transport = std::string ())
{
    if (zlink_connect (socket_, endpoint.c_str ()) != ZLINK_CONNECT_OK) {
        std::cerr << "connect failed for " << endpoint << ": " << zlink_strerror (zlink_errno ())
                  << std::endl;
        return false;
    }
    apply_debug_timeouts (socket_,
                          transport.empty () ? transport_from_endpoint (endpoint) : transport);
    if (bench_debug_enabled ()) {
        std::cerr << "Connected to " << endpoint << std::endl;
    }
    return true;
}

inline std::vector<size_t> resolve_bench_msg_sizes (size_t fallback_size)
{
    const size_t default_size = fallback_size > 0 ? fallback_size : 64;
    std::vector<size_t> sizes;

    if (const char *env = std::getenv ("PERF_MSG_SIZES")) {
        const char *cur = env;
        while (*cur) {
            while (*cur == ',' || *cur == ' ' || *cur == '\t')
                ++cur;
            if (!*cur)
                break;

            errno = 0;
            char *end = NULL;
            const unsigned long parsed = std::strtoul (cur, &end, 10);
            if (errno == 0 && end != cur && parsed > 0)
                sizes.push_back (static_cast<size_t> (parsed));

            if (!end || end == cur)
                break;
            cur = end;
            while (*cur && *cur != ',')
                ++cur;
            if (*cur == ',')
                ++cur;
        }
    }

    if (sizes.empty ())
        sizes.push_back (default_size);
    return sizes;
}

inline size_t perf_current_benchmark_max_msg_size (size_t fallback_size)
{
    const std::vector<size_t> sizes = resolve_bench_msg_sizes (fallback_size);
    size_t max_msg_size = fallback_size > 0 ? fallback_size : 64;
    for (size_t i = 0; i < sizes.size (); ++i) {
        if (sizes[i] > max_msg_size)
            max_msg_size = sizes[i];
    }
    return max_msg_size;
}

inline std::atomic<bool> &perf_stop_requested ()
{
    static std::atomic<bool> flag (false);
    return flag;
}

inline void perf_on_signal (int)
{
    perf_stop_requested ().store (true, std::memory_order_release);
}

inline void install_perf_signal_handlers ()
{
    std::signal (SIGINT, perf_on_signal);
#if defined(SIGTERM)
    std::signal (SIGTERM, perf_on_signal);
#endif
}

inline bool is_supported_transport (const std::string &transport_)
{
    return perf_supports_service_transport (transport_);
}

inline std::string
bind_server_endpoint (void *server_, const std::string &transport_, const std::string &token_)
{
    const int bind_port = resolve_multi_int_env ("PERF_MULTI_SERVER_BIND_PORT", 0, 0);
    std::string endpoint = bind_port > 0 ? make_fixed_endpoint (transport_, bind_port)
                                         : make_endpoint (transport_, token_);
    if (endpoint.empty ()) {
        std::cerr << "No endpoint available for transport " << transport_ << std::endl;
        return std::string ();
    }

    endpoint =
      perf_bind_endpoint_once (server_, endpoint, transport_, &perf_bind_socket_endpoint, true);
    if (endpoint.empty ())
        return std::string ();
    apply_debug_timeouts (server_, transport_);
    return endpoint;
}

#endif

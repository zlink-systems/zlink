/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

namespace zlink::detail
{

enum class context_option_id : int
{
    io_threads = 1,
    max_sockets = 2,
    socket_limit = 3,
    thread_priority = 3,
    thread_sched_policy = 4,
    max_msgsz = 5,
    msg_t_size = 6,
    thread_affinity_cpu_add = 7,
    thread_affinity_cpu_remove = 8,
    thread_name_prefix = 9,
    blocky = 10,
    auto_hwm_enable = 12,
    auto_hwm_recalc_debounce_ms = 14,
    auto_hwm_profile = 17,
    auto_hwm_msg_unit_bytes = 18
};

enum class socket_option_id : int
{
    linger = 12298,
    reconnect_ivl = 12299,
    backlog = 12300,
    reconnect_ivl_max = 12301,
    maxmsgsize = 12302,
    sndhwm = 12303,
    rcvhwm = 12304,
    rcvtimeo = 12306,
    sndtimeo = 12307,
    last_endpoint = 12308,
    tcp_keepalive = 12309,
    tcp_nodelay = 12337,
    immediate = 12313,
    ipv6 = 12314,
    submit_retry_mode = 12343,
    submit_retry_timeout = 12344,
    submit_retry_attempts = 12345,
    connect_timeout = 12324,
    rid_duplicate_policy = 12339
};

enum class router_option_id : int
{
    mandatory = 12545,
    probe = 12547,
    connect_routing_id = 12548,
    request_timeout_ms = 12549,
    weight = 12550
};

enum class dealer_option_id : int
{
    probe = 12801,
    request_timeout_ms = 12802,
    weight = 12803
};

enum class pub_option_id : int
{
    verbose = 13057,
    verboser = 13058,
    manual = 13059,
    manual_last_value = 13060,
    nodrop = 13061,
    welcome_msg = 13062,
    topics_count = 13063,
    approve_subscribe = 13064,
    reject_subscribe = 13065
};

enum class sub_option_id : int
{
    topics_count = 13312
};

enum class stream_option_id : int
{
    notify = 13569
};

} // namespace zlink::detail

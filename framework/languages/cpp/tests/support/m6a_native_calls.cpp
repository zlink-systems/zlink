/* SPDX-License-Identifier: Elastic-2.0 */

#include "m6a_native_calls.hpp"
#include <cerrno>
#include <cstdlib>

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

#if !defined(_WIN32)
// Linux interposes Core entry points for binding failure injection and submission counts.
namespace zlink::framework::test::m6a
{
std::atomic_bool count_request_submissions{false};
std::atomic_int request_submissions{0};
std::atomic_bool fail_next_disconnect_rid{false};
std::atomic_int injected_disconnect_rid_calls{0};
std::atomic<zlink_connect_result_t> injected_disconnect_result{ZLINK_CONNECT_INTERNAL_ERROR};

using disconnect_rid_fn_t = zlink_connect_result_t (*) (void *, const zlink_routing_id_t *);

disconnect_rid_fn_t native_disconnect_rid ()
{
    static const auto function =
      reinterpret_cast<disconnect_rid_fn_t> (dlsym (RTLD_NEXT, "zlink_disconnect_rid"));
    if (!function)
        std::abort ();
    return function;
}
} // namespace

using namespace zlink::framework::test::m6a;

extern "C" zlink_submit_result_t zlink_request (void *socket,
                                                const zlink_routing_id_t *target,
                                                zlink_msg_t *parts,
                                                size_t part_count,
                                                zlink_send_flags_t flags,
                                                uint32_t timeout_ms,
                                                void *context,
                                                zlink_completion_id_t *completion)
{
    using request_fn_t = decltype (&zlink_request);
    static const auto request = reinterpret_cast<request_fn_t> (dlsym (RTLD_NEXT, "zlink_request"));
    if (!request)
        std::abort ();
    if (count_request_submissions.load (std::memory_order_acquire))
        request_submissions.fetch_add (1, std::memory_order_relaxed);
    return request (socket, target, parts, part_count, flags, timeout_ms, context, completion);
}

extern "C" zlink_connect_result_t zlink_disconnect_rid (void *socket,
                                                        const zlink_routing_id_t *peer_rid)
{
    if (fail_next_disconnect_rid.exchange (false, std::memory_order_acq_rel)) {
        injected_disconnect_rid_calls.fetch_add (1, std::memory_order_relaxed);
        errno = EIO;
        return injected_disconnect_result.load (std::memory_order_relaxed);
    }
    return native_disconnect_rid () (socket, peer_rid);
}
#endif

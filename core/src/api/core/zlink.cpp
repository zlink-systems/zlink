/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#define ZLINK_TYPE_UNSAFE

#include "utils/macros.hpp"
#include "utils/random.hpp"

#if !defined ZLINK_HAVE_WINDOWS
#include <unistd.h>
#ifdef ZLINK_HAVE_VXWORKS
#include <strings.h>
#endif
#endif

// XSI vector I/O
#if defined ZLINK_HAVE_UIO
#include <sys/uio.h>
#else
struct iovec
{
    void *iov_base;
    size_t iov_len;
};
#endif

#include <string.h>
#include <stdlib.h>
#include <new>

#include "sockets/proxy/proxy.hpp"
#include "sockets/common/socket_base.hpp"
#include "api/monitoring/monitor_api_internal.hpp"
#include "api/socket/part_helper_internal.hpp"
#include "api/monitoring/poller_api_internal.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
#include "api/core/close_result_internal.hpp"
#include "api/core/config_result_internal.hpp"
#include "api/core/zlink_option_internal.hpp"
#include "utils/mutex.hpp"
#include "utils/stdint.hpp"
#include "utils/config.hpp"
#include "utils/clock.hpp"
#include "utils/sleep.hpp"
#include "core/ctx.hpp"
#include "utils/err.hpp"
#include "core/socket_poller.hpp"
#include "utils/fd.hpp"
#include "protocol/metadata.hpp"
#include "utils/ip.hpp"
#include "core/address.hpp"

#ifdef ZLINK_HAVE_PPOLL
#include "utils/polling_util.hpp"
#include <sys/select.h>
#endif

//  Compile time check whether msg_t fits into zlink_msg_t.
typedef char check_msg_t_size[sizeof (zlink::msg_t) == sizeof (zlink_msg_t) ? 1 : -1];

//  Forward declarations for internal API functions
zlink_config_result_t zlink_msg_init_buffer (zlink_msg_t *msg_, const void *buf_, size_t size_);
int zlink_ctx_set_ext (void *ctx_, int option_, const void *optval_, size_t optvallen_);
zlink_config_result_t zlink_set_subscription (void *handle_, const char *filter_);
zlink_config_result_t zlink_unset_subscription (void *handle_, const char *filter_);
static void *create_socket_handle (void *ctx_, zlink_socket_type_t type_)
{
    if (!ctx_ || !(static_cast<zlink::ctx_t *> (ctx_))->check_tag ()) {
        errno = EFAULT;
        return NULL;
    }

    const int core_type = core_socket_type_from_public_type (type_);
    if (!is_send_only_socket_type (core_type)) {
        switch (core_type) {
            case ZLINK_CORE_SOCKET_PAIR:
            case ZLINK_CORE_SOCKET_DEALER:
            case ZLINK_CORE_SOCKET_ROUTER:
            case ZLINK_CORE_SOCKET_STREAM:
            case ZLINK_CORE_SOCKET_SUB:
            case ZLINK_CORE_SOCKET_XSUB:
            case ZLINK_CORE_SOCKET_XPUB:
                break;
            default:
                errno = EINVAL;
                return NULL;
        }
    }

    zlink::ctx_t *ctx = static_cast<zlink::ctx_t *> (ctx_);
    zlink::socket_base_t *socket = ctx->create_socket (core_type);
    if (!socket)
        return NULL;

    void *public_handle = ctx->register_public_socket_handle (socket);
    if (!public_handle) {
        const int saved_errno = errno;
        socket->close ();
        errno = saved_errno;
        return NULL;
    }
    return public_handle;
}

extern "C" void zlink_socket_request_reply_cleanup (void *socket_);

void *zlink_socket (void *ctx_, zlink_socket_type_t type_)
{
    return create_socket_handle (ctx_, type_);
}

zlink_close_result_t zlink_close (void *s_)
{
    socket_handle_t handle = as_socket_handle (s_);
    if (!handle.socket)
        return zlink::close_result_internal::from_errno (errno);
    std::shared_ptr<zlink::socket_reqrep_internal::socket_request_reply_state_t>
      request_reply_state = zlink::socket_reqrep_internal::find_request_reply_state (handle);
    // Reserve close before stopping the socket or clearing multipart/request
    // state so new public operations cannot enter during teardown.
    if (!handle.begin_close ())
        return zlink::close_result_internal::from_rc (-1);
    const int close_admission = handle.socket->begin_close_handoff ();
    if (close_admission < 0) {
        handle.cancel_close ();
        return zlink::close_result_internal::from_rc (-1);
    }
    const bool deferred_close = close_admission > 0;

    {
        monitor_pull_state_pin_t monitor_pin (handle.socket);
        monitor_pull_state_t *monitor_state = monitor_pin.get ();
        socket_handle_t monitor_source =
          monitor_snapshot_subject_handle (monitor_state);
        if (monitor_source.socket && monitor_source.socket != handle.socket) {
            monitor_state->snapshot_subject.store (NULL, std::memory_order_release);
            (void) monitor_source.socket->monitor (NULL, 0, 3,
                                                   ZLINK_CORE_SOCKET_PAIR, 0);
        } else {
            clear_raw_monitor_snapshot_subjects (handle.socket);
        }
    }

    unregister_monitor_pull_state (handle.socket);
    zlink::part_helper_internal::cleanup_socket (handle.socket);

    if (handle.socket->api_sync_mutex ()) {
        handle.socket->stop ();
        {
            // Close admission already rejects new public operations. Hold the
            // STREAM API mutex only long enough to quiesce an operation that
            // entered before admission. Async pipe termination re-enters the
            // same mutex, so release it before waiting for async quiescence.
            stream_api_lock_t api_lock (handle);
        }
        const int drain_rc =
          zlink::socket_reqrep_internal::drain_close_request_reply_socket (handle);
        const int drain_errno = errno;
        zlink::socket_reqrep_internal::cleanup_request_reply_socket (handle);
        if (!deferred_close)
            handle.socket->complete_close_handoff ();
        if (drain_rc < 0) {
            errno = drain_errno;
            return zlink::close_result_internal::from_rc (-1);
        }
        return ZLINK_CLOSE_OK;
    }

    const int drain_rc =
      zlink::socket_reqrep_internal::drain_close_request_reply_socket (handle);
    const int drain_errno = errno;
    zlink::socket_reqrep_internal::cleanup_request_reply_socket (handle);
    if (!deferred_close)
        handle.socket->complete_close_handoff ();
    if (drain_rc < 0) {
        errno = drain_errno;
        return zlink::close_result_internal::from_rc (-1);
    }
    return ZLINK_CLOSE_OK;
}

zlink_config_result_t
zlink_poller_add (void *poller_, void *socket_, void *user_data_, short events_)
{
    poller_api_guard_t guard (poller_);
    poller_handle_t *poller = guard.get ();
    if (!poller)
        return ZLINK_CONFIG_INVALID_HANDLE;
    std::lock_guard<std::mutex> operation_lock (poller->operation_sync);
    if (poller->wait_active) {
        errno = EBUSY;
        return ZLINK_CONFIG_BUSY;
    }
    const zlink::option_target_t target = zlink::resolve_option_target (socket_);
    if (target.kind != zlink::option_target_socket)
        return zlink::config_result_internal::from_errno (errno);
    socket_handle_t handle = make_socket_handle (target.socket);
    const int type = socket_type (handle);
    //  A completion registration is valid on any socket that owns a
    //  completion channel: request/reply completions (DEALER/ROUTER) or SEND
    //  completions (PAIR/DEALER/ROUTER/STREAM). Registration transfers pull
    //  ownership of both to the poller wait thread.
    const bool has_completion_channel =
      type == ZLINK_CORE_SOCKET_DEALER || type == ZLINK_CORE_SOCKET_ROUTER
      || zlink::socket_type_supports_completion_pull (type);
    if (validate_socket_poller_event_mask (events_, has_completion_channel)
        != 0)
        return zlink::config_result_internal::from_errno (errno);
    if ((events_ & ZLINK_POLLCOMPLETION) != 0 && !has_completion_channel) {
        errno = EINVAL;
        return ZLINK_CONFIG_INVALID_ARGUMENT;
    }
    if (poller_find_registration_index (poller, socket_, poller_subject_none)
        >= 0) {
        errno = EEXIST;
        return ZLINK_CONFIG_CONFLICT;
    }
    if (poller_add_registration (poller, handle.socket, user_data_, events_, socket_,
                                 poller_subject_none)
        != 0) {
        return zlink::config_result_internal::from_errno (errno);
    }
    if ((events_ & ZLINK_POLLCOMPLETION) != 0 && has_completion_channel) {
        if (!handle.socket->acquire_completion_poller (poller)) {
            const int acquire_errno = errno;
            const int registration_index = poller_find_registration_index (
              poller, socket_, poller_subject_none);
            if (registration_index >= 0)
                (void) poller_remove_registration_at (poller,
                                                       registration_index);
            errno = acquire_errno;
            return zlink::config_result_internal::from_errno (errno);
        }
        poller->registrations.back ().owns_completion_processing = true;
        poller->registrations.back ().completion_owner = poller;
    }

    return ZLINK_CONFIG_OK;
}

zlink_config_result_t zlink_poller_modify (void *poller_, void *socket_, short events_)
{
    poller_api_guard_t guard (poller_);
    poller_handle_t *poller = guard.get ();
    if (!poller)
        return ZLINK_CONFIG_INVALID_HANDLE;
    std::lock_guard<std::mutex> operation_lock (poller->operation_sync);
    if (poller->wait_active) {
        errno = EBUSY;
        return ZLINK_CONFIG_BUSY;
    }
    const zlink::option_target_t target = zlink::resolve_option_target (socket_);
    if (target.kind != zlink::option_target_socket)
        return zlink::config_result_internal::from_errno (errno);
    socket_handle_t handle = make_socket_handle (target.socket);
    const int type = socket_type (handle);
    const bool has_completion_channel =
      type == ZLINK_CORE_SOCKET_DEALER || type == ZLINK_CORE_SOCKET_ROUTER
      || zlink::socket_type_supports_completion_pull (type);
    if (validate_socket_poller_event_mask (events_, has_completion_channel)
        != 0)
        return zlink::config_result_internal::from_errno (errno);
    const int index =
      poller_find_registration_index (poller, socket_, poller_subject_none);
    if (index < 0) {
        errno = ENOENT;
        return ZLINK_CONFIG_NOT_FOUND;
    }
    poller_registration_t &registration = poller->registrations[index];
    const bool had_completion = registration.owns_completion_processing;
    const bool wants_completion = (events_ & ZLINK_POLLCOMPLETION) != 0;
    if (wants_completion && !had_completion
        && !handle.socket->acquire_completion_poller (poller))
        return zlink::config_result_internal::from_errno (errno);
    if (poller->poller.modify (
          static_cast<zlink::socket_base_t *> (poller->registrations[index].socket),
          events_)
        != 0) {
        if (wants_completion && !had_completion)
            handle.socket->release_completion_poller (poller);
        return zlink::config_result_internal::from_errno (errno);
    }
    registration.events = events_;
    if (wants_completion && !had_completion) {
        registration.owns_completion_processing = true;
        registration.completion_owner = poller;
    } else if (!wants_completion && had_completion) {
        handle.socket->release_completion_poller (registration.completion_owner);
        registration.owns_completion_processing = false;
        registration.completion_owner = NULL;
    }
    return ZLINK_CONFIG_OK;
}

zlink_config_result_t zlink_poller_remove (void *poller_, void *socket_)
{
    poller_api_guard_t guard (poller_);
    poller_handle_t *poller = guard.get ();
    if (!poller)
        return ZLINK_CONFIG_INVALID_HANDLE;
    std::lock_guard<std::mutex> operation_lock (poller->operation_sync);
    if (poller->wait_active) {
        errno = EBUSY;
        return ZLINK_CONFIG_BUSY;
    }
    // A socket may have completed its public close while its poller
    // registration still owns the lifetime pin. In that state the public
    // handle registry no longer accepts the raw pointer, but removal is
    // still valid because the poller registration table is the authority.
    // Only use the raw pointer after finding the exact registered subject;
    // arbitrary invalid pointers still go through normal handle validation.
    if (poller_find_registration_index (poller, socket_, poller_subject_none) < 0) {
        const zlink::option_target_t target = zlink::resolve_option_target (socket_);
        if (target.kind != zlink::option_target_socket)
            return zlink::config_result_internal::from_errno (errno);
    }
    return zlink::config_result_internal::from_rc (
      poller_remove_all_registrations_for_subject (poller, socket_));
}

/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "sockets/stream/stream.hpp"
#include "sockets/stream/stream_dispatch_internal.hpp"
#include "sockets/stream/stream_dispatch_send_policy_internal.hpp"
#include "core/pipe.hpp"
#include "utils/err.hpp"

int zlink::stream_t::stream_dispatch_send_from_io (const zlink_routing_id_t *rid_,
                                                   const void *data_,
                                                   size_t size_,
                                                   int flags_)
{
    if (!rid_ || rid_->size != 4) {
        errno = EINVAL;
        return -1;
    }
    if (!data_ && size_ > 0) {
        errno = EINVAL;
        return -1;
    }

    const uint32_t routing_id = stream_dispatch_decode_routing_id (rid_);
    if (routing_id == 0) {
        errno = EINVAL;
        return -1;
    }

    pipe_t *const direct_out = resolve_direct_dispatch_output_pipe (this, routing_id);
    if (size_ == 0) {
        if (direct_out) {
            direct_out->terminate (false);
            return 1;
        }

        route_shard_t &shard = route_shard_for (routing_id);
        scoped_fast_lock_t shard_lock (shard.sync);
        route_shard_t::routes_t::iterator it = shard.routes.find (routing_id);
        if (it == shard.routes.end () || !it->second) {
            errno = EHOSTUNREACH;
            return -1;
        }
        it->second->terminate (false);
        return 1;
    }

    msg_t out_msg;
    if (out_msg.init_buffer (data_, size_) != 0)
        return -1;

    if (direct_out
        && direct_out->write_single_message_and_flush_no_recursive_hwm_check (&out_msg)) {
        const int init_rc = out_msg.init ();
        errno_assert (init_rc == 0);
        return 1;
    }

    const stream_dispatch_send_policy_t policy (flags_, options.sndtimeo);

    for (;;) {
        {
            route_shard_t &shard = route_shard_for (routing_id);
            scoped_fast_lock_t shard_lock (shard.sync);
            route_shard_t::routes_t::iterator it = shard.routes.find (routing_id);
            if (it == shard.routes.end () || !it->second) {
                const int rc = out_msg.close ();
                errno_assert (rc == 0);
                errno = EHOSTUNREACH;
                return -1;
            }

            if (it->second->write_single_message_and_flush_no_recursive_hwm_check (&out_msg)) {
                const int init_rc = out_msg.init ();
                errno_assert (init_rc == 0);
                return 1;
            }
        }

        if (policy.dontwait ()) {
            const int rc = out_msg.close ();
            errno_assert (rc == 0);
            errno = EAGAIN;
            return -1;
        }

        if (policy.timed_out ()) {
            const int rc = out_msg.close ();
            errno_assert (rc == 0);
            errno = EAGAIN;
            return -1;
        }

        if (!policy.wait_retry ())
            break;
    }

    const int rc = out_msg.close ();
    errno_assert (rc == 0);
    errno = EAGAIN;
    return -1;
}

int zlink::stream_t::stream_dispatch_send_msg_from_io (const zlink_routing_id_t *rid_,
                                                       msg_t *msg_,
                                                       int flags_)
{
    if (!rid_ || rid_->size != 4 || !msg_) {
        errno = EINVAL;
        return -1;
    }

    const uint32_t routing_id = stream_dispatch_decode_routing_id (rid_);
    if (routing_id == 0) {
        errno = EINVAL;
        return -1;
    }

    pipe_t *const direct_out = resolve_direct_dispatch_output_pipe (this, routing_id);
    if (direct_out) {
        if (msg_->size () == 0) {
            direct_out->terminate (false);
            const int init_rc = msg_->init ();
            errno_assert (init_rc == 0);
            return 1;
        }

        if (direct_out->write_single_message_and_flush_no_recursive_hwm_check (msg_)) {
            const int init_rc = msg_->init ();
            errno_assert (init_rc == 0);
            return 1;
        }
    }

    const stream_dispatch_send_policy_t policy (flags_, options.sndtimeo);

    for (;;) {
        {
            route_shard_t &shard = route_shard_for (routing_id);
            scoped_fast_lock_t shard_lock (shard.sync);
            route_shard_t::routes_t::iterator it = shard.routes.find (routing_id);
            if (it == shard.routes.end () || !it->second) {
                errno = EHOSTUNREACH;
                return -1;
            }

            if (msg_->size () == 0) {
                it->second->terminate (false);
                const int init_rc = msg_->init ();
                errno_assert (init_rc == 0);
                return 1;
            }

            if (it->second->write_single_message_and_flush_no_recursive_hwm_check (msg_)) {
                const int init_rc = msg_->init ();
                errno_assert (init_rc == 0);
                return 1;
            }
        }

        if (policy.dontwait ()) {
            errno = EAGAIN;
            return -1;
        }

        if (policy.timed_out ()) {
            errno = EAGAIN;
            return -1;
        }

        if (!policy.wait_retry ())
            break;
    }

    errno = EAGAIN;
    return -1;
}

int zlink::stream_t::stream_dispatch_send_current_msg_from_io (msg_t *msg_, int flags_)
{
    if (!msg_) {
        errno = EINVAL;
        return -1;
    }

    pipe_t *dispatch_pipe = zlink::stream_dispatch_context_t::current_pipe ();
    pipe_t *direct_out = resolve_current_dispatch_output_pipe ();
    if (!direct_out) {
        errno = EAGAIN;
        return -1;
    }

    if (msg_->size () == 0) {
        direct_out->terminate (false);
        const int init_rc = msg_->init ();
        errno_assert (init_rc == 0);
        return 1;
    }

    if (direct_out->write_single_message_and_flush_no_recursive_hwm_check (msg_)) {
        const int init_rc = msg_->init ();
        errno_assert (init_rc == 0);
        return 1;
    }

    if (dispatch_pipe) {
        direct_out->refresh_write_credit (dispatch_pipe->get_msgs_read (),
                                          dispatch_pipe->get_bytes_read ());
        if (direct_out->write_single_message_and_flush_no_recursive_hwm_check (msg_)) {
            const int init_rc = msg_->init ();
            errno_assert (init_rc == 0);
            return 1;
        }
    }

    LIBZLINK_UNUSED (flags_);
    errno = EAGAIN;
    return -1;
}

/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include <string.h>
#include <vector>

#include "utils/macros.hpp"
#include "sockets/pubsub/xsub.hpp"
#include "utils/err.hpp"
#include "utils/env.hpp"

namespace
{
const int xsub_transport_read_batch_size = 128 * 1024;

unsigned int resolve_non_matching_skip_budget ()
{
    return zlink::env::positive_uint ("ZLINK_XSUB_NON_MATCHING_SKIP_BUDGET", 64u);
}

const unsigned int xsub_non_matching_skip_budget = resolve_non_matching_skip_budget ();

struct xsub_snapshot_arg_t
{
    explicit xsub_snapshot_arg_t (std::vector<zlink::xsub_t::subscription_descriptor_t> *out_) :
        out (out_)
    {
    }

    std::vector<zlink::xsub_t::subscription_descriptor_t> *out;
};

static void snapshot_subscription (unsigned char *data_, size_t size_, void *arg_)
{
    xsub_snapshot_arg_t *arg = static_cast<xsub_snapshot_arg_t *> (arg_);
    if (!arg || !arg->out)
        return;

    zlink::xsub_t::subscription_descriptor_t item;
    item.filter.assign (reinterpret_cast<const char *> (data_), size_);
    item.is_pattern = false;
    arg->out->push_back (item);
}
}

zlink::xsub_t::xsub_t (class ctx_t *parent_, uint32_t tid_, int sid_) :
    socket_base_t (parent_, tid_, sid_),
    _verbose_unsubs (false),
    _has_message (false),
    _more_send (false),
    _more_recv (false),
    _recv_part_index (0),
    _recv_protocol_error_pending (false),
    _process_subscribe (false),
    _has_empty_subscription (false),
    _delivery_ready_count (0)
{
    options.type = ZLINK_CORE_SOCKET_XSUB;
    options.in_batch_size = xsub_transport_read_batch_size;
    refresh_auto_hwm_policy ();

    //  When socket is being closed down we don't want to wait till pending
    //  subscription commands are sent to the wire.
    options.linger.store (0);

    const int rc = _message.init ();
    errno_assert (rc == 0);
}

bool zlink::xsub_t::compute_delivery_ready_state () const
{
    return compute_delivery_ready_count () > 0;
}

uint32_t zlink::xsub_t::compute_delivery_ready_count () const
{
    std::lock_guard<std::mutex> subscriptions_lock (_subscriptions_mu);
#ifdef ZLINK_USE_RADIX_TREE
    const bool has_filters = _subscriptions.size () > 0;
#else
    const bool has_filters = _subscriptions.num_prefixes () > 0;
#endif
    if (!has_filters)
        return 0;
    return has_attached_pipes () ? 1u : 0u;
}

void zlink::xsub_t::refresh_delivery_ready_state (const endpoint_uri_pair_t &endpoint_uri_pair_)
{
    LIBZLINK_UNUSED (endpoint_uri_pair_);
    const uint32_t ready_count = compute_delivery_ready_count ();
    _delivery_ready_count.store (ready_count, std::memory_order_release);
}

uint32_t zlink::xsub_t::monitor_ready_count () const
{
    return _delivery_ready_count.load (std::memory_order_acquire);
}

zlink::xsub_t::~xsub_t ()
{
    const int rc = _message.close ();
    errno_assert (rc == 0);
}

void zlink::xsub_t::snapshot_subscriptions (std::vector<subscription_descriptor_t> *out_) const
{
    if (!out_)
        return;

    xsub_snapshot_arg_t arg (out_);
    std::lock_guard<std::mutex> subscriptions_lock (_subscriptions_mu);
    _subscriptions.apply (&snapshot_subscription, &arg);
}

void zlink::xsub_t::xattach_pipe (pipe_t *pipe_, bool subscribe_to_all_, bool locally_initiated_)
{
    LIBZLINK_UNUSED (subscribe_to_all_);
    LIBZLINK_UNUSED (locally_initiated_);

    zlink_assert (pipe_);
    _fq.attach (pipe_);
    (void) pipe_->check_read ();
    _dist.attach (pipe_);

    //  Send all the cached subscriptions to the new upstream peer.
    std::vector<subscription_descriptor_t> subscriptions;
    snapshot_subscriptions (&subscriptions);
    for (size_t i = 0; i < subscriptions.size (); ++i) {
        unsigned char *data = subscriptions[i].filter.empty ()
                                ? NULL
                                : reinterpret_cast<unsigned char *> (&subscriptions[i].filter[0]);
        send_subscription (data, subscriptions[i].filter.size (), pipe_);
    }
    pipe_->flush ();
    refresh_delivery_ready_state (pipe_->get_endpoint_pair ());
}

void zlink::xsub_t::xread_activated (pipe_t *pipe_)
{
    _fq.activated (pipe_);
}

void zlink::xsub_t::xwrite_activated (pipe_t *pipe_)
{
    _dist.activated (pipe_);
    if (pipe_)
        refresh_delivery_ready_state (pipe_->get_endpoint_pair ());
}

void zlink::xsub_t::xpipe_terminated (pipe_t *pipe_)
{
    const endpoint_uri_pair_t endpoint_pair =
      pipe_ ? pipe_->get_endpoint_pair () : endpoint_uri_pair_t ();
    _fq.pipe_terminated (pipe_);
    _dist.pipe_terminated (pipe_);
    refresh_delivery_ready_state (endpoint_pair);
}

void zlink::xsub_t::xhiccuped (pipe_t *pipe_)
{
    //  Send all the cached subscriptions to the hiccuped pipe.
    std::vector<subscription_descriptor_t> subscriptions;
    snapshot_subscriptions (&subscriptions);
    for (size_t i = 0; i < subscriptions.size (); ++i) {
        unsigned char *data = subscriptions[i].filter.empty ()
                                ? NULL
                                : reinterpret_cast<unsigned char *> (&subscriptions[i].filter[0]);
        send_subscription (data, subscriptions[i].filter.size (), pipe_);
    }
    pipe_->flush ();
}

int zlink::xsub_t::xsetsockopt (int option_, const void *optval_, size_t optvallen_)
{
    errno = EINVAL;
    return -1;
}

int zlink::xsub_t::xgetsockopt (int option_, void *optval_, size_t *optvallen_)
{
    if (option_ == ZLINK_INTERNAL_OPT_TOPICS_COUNT) {
        // make sure to use a multi-thread safe function to avoid race conditions with I/O threads
        // where subscriptions are processed:
        std::lock_guard<std::mutex> subscriptions_lock (_subscriptions_mu);
#ifdef ZLINK_USE_RADIX_TREE
        uint64_t num_subscriptions = _subscriptions.size ();
#else
        uint64_t num_subscriptions = _subscriptions.num_prefixes ();
#endif

        return do_getsockopt<int> (optval_, optvallen_, (int) num_subscriptions);
    }

    // room for future options here

    errno = EINVAL;
    return -1;
}

int zlink::xsub_t::xsend (
  msg_t *msg_, pipe_message_admission_t *admission_out_)
{
    if (admission_out_)
        *admission_out_ = pipe_message_admission_ready;
    size_t size = msg_->size ();
    unsigned char *data = static_cast<unsigned char *> (msg_->data ());

    const bool first_part = !_more_send;
    _more_send = (msg_->flags () & msg_t::more) != 0;

    if (first_part) {
        _process_subscribe = true;
    } else if (!_process_subscribe) {
        //  User message sent upstream to XPUB socket
        return _dist.send_to_all (msg_);
    }

    if (msg_->is_subscribe () || (size > 0 && *data == 1)) {
        //  Process subscribe message
        //  This used to filter out duplicate subscriptions,
        //  however this is already done on the XPUB side and
        //  doing it here as well breaks ZLINK_INTERNAL_OPT_XPUB_VERBOSE
        //  when there are forwarding devices involved.
        if (!msg_->is_subscribe ()) {
            data = data + 1;
            size = size - 1;
        }
        {
            std::lock_guard<std::mutex> subscriptions_lock (_subscriptions_mu);
            _subscriptions.add (data, size);
            if (size == 0)
                _has_empty_subscription.store (true, std::memory_order_release);
        }
        _process_subscribe = true;
        const int rc = _dist.send_to_all (msg_);
        refresh_delivery_ready_state (endpoint_uri_pair_t ());
        return rc;
    }
    if (msg_->is_cancel () || (size > 0 && *data == 0)) {
        //  Process unsubscribe message
        if (!msg_->is_cancel ()) {
            data = data + 1;
            size = size - 1;
        }
        _process_subscribe = true;
        bool rm_result = false;
        {
            std::lock_guard<std::mutex> subscriptions_lock (_subscriptions_mu);
            rm_result = _subscriptions.rm (data, size);
            if (size == 0 && rm_result)
                _has_empty_subscription.store (false, std::memory_order_release);
        }
        if (rm_result || _verbose_unsubs) {
            const int rc = _dist.send_to_all (msg_);
            refresh_delivery_ready_state (endpoint_uri_pair_t ());
            return rc;
        }
    } else
        //  User message sent upstream to XPUB socket
        return _dist.send_to_all (msg_);

    int rc = msg_->close ();
    errno_assert (rc == 0);
    rc = msg_->init ();
    errno_assert (rc == 0);
    refresh_delivery_ready_state (endpoint_uri_pair_t ());

    return 0;
}

bool zlink::xsub_t::xhas_out ()
{
    //  Subscription can be added/removed anytime.
    return true;
}

int zlink::xsub_t::xrecv (msg_t *msg_)
{
    if (_recv_protocol_error_pending) {
        _recv_protocol_error_pending = false;
        errno = EPROTO;
        return -1;
    }

    //  If there's already a message prepared by a previous call to zlink_poll,
    //  return it straight ahead.
    if (_has_message) {
        const int rc = msg_->move (_message);
        errno_assert (rc == 0);
        _has_message = false;
        _more_recv = (msg_->flags () & msg_t::more) != 0;
        _recv_part_index = _more_recv ? 1 : 0;
        msg_->reset_request_reply_metadata ();
        return 0;
    }

    //  Bound the number of non-matching messages skipped in one call so
    //  non-blocking recv semantics still return control under hostile traffic.
    unsigned int skipped_non_matching = 0;
    while (true) {
        //  Get a message using fair queueing algorithm.
        pipe_t *pipe = NULL;
        const bool continuing_exposed_message = _more_recv;
        int rc = _fq.recvpipe (msg_, &pipe);

        //  If there's no message available, return immediately.
        //  The same when error occurs.
        if (rc != 0) {
            if (errno == ECONNABORTED) {
                _more_recv = false;
                _recv_part_index = 0;
                if (!continuing_exposed_message)
                    continue;
            }
            return -1;
        }

        //  Check whether the message matches at least one subscription.
        //  Non-initial parts of the message are passed
        const bool first_part = !_more_recv;
        const size_t part_index = first_part ? 0 : _recv_part_index;
        unsigned char request_reply_kind = 0;
        uint64_t request_reply_sequence = 0;
        if (part_index > 0
            && msg_->get_request_reply_metadata (
              &request_reply_kind, &request_reply_sequence)) {
            pipe_t *const malformed_pipe =
              pipe && pipe->retain_lifetime_ref () ? pipe : NULL;
            bool more = (msg_->flags () & msg_t::more) != 0;
            while (more) {
                rc = _fq.recvpipe (msg_, &pipe);
                if (rc != 0)
                    break;
                more = (msg_->flags () & msg_t::more) != 0;
            }
            if (malformed_pipe) {
                malformed_pipe->terminate (false);
                malformed_pipe->release_lifetime_ref ();
            }
            const int close_rc = msg_->close ();
            errno_assert (close_rc == 0);
            const int init_rc = msg_->init ();
            errno_assert (init_rc == 0);
            _more_recv = false;
            _recv_part_index = 0;
            errno = EPROTO;
            return -1;
        }
        if (_more_recv || !options.filter || match (msg_)) {
            _more_recv = (msg_->flags () & msg_t::more) != 0;
            _recv_part_index = _more_recv ? part_index + 1 : 0;
            if (first_part)
                msg_->reset_request_reply_metadata ();
            return 0;
        }

        // Structural validation is independent of subscription matching: an
        // inproc-only malformed record must be rejected just like a decoded
        // network record, even when its topic would otherwise be discarded.
        rc = discard_filtered_message (msg_, pipe);
        if (rc != 0) {
            if (errno == ECONNABORTED) {
                _more_recv = false;
                _recv_part_index = 0;
                continue;
            }
            const int saved_errno = errno;
            const int close_rc = msg_->close ();
            errno_assert (close_rc == 0);
            const int init_rc = msg_->init ();
            errno_assert (init_rc == 0);
            errno = saved_errno;
            return -1;
        }
        ++skipped_non_matching;
        if (unlikely (skipped_non_matching >= xsub_non_matching_skip_budget)) {
            errno = EAGAIN;
            return -1;
        }
    }
}

bool zlink::xsub_t::xhas_in ()
{
    if (_recv_protocol_error_pending)
        return true;

    //  There are subsequent parts of the partly-read message available.
    if (_more_recv)
        return true;

    //  If there's already a message prepared by a previous call to zlink_poll,
    //  return straight ahead.
    if (_has_message)
        return true;

    //  Bound the number of non-matching messages skipped in one probe so a
    //  caller polling for readiness is not trapped by unrelated traffic.
    unsigned int skipped_non_matching = 0;
    while (true) {
        //  Get a message using fair queueing algorithm.
        pipe_t *pipe = NULL;
        int rc = _fq.recvpipe (&_message, &pipe);

        //  If there's no message available, return immediately.
        //  The same when error occurs.
        if (rc != 0) {
            if (errno == ECONNABORTED) {
                _more_recv = false;
                _recv_part_index = 0;
                continue;
            }
            errno_assert (errno == EAGAIN);
            return false;
        }

        //  Check whether the message matches at least one subscription.
        if (!options.filter || match (&_message)) {
            _has_message = true;
            return true;
        }

        rc = discard_filtered_message (&_message, pipe);
        if (rc != 0) {
            if (errno == ECONNABORTED) {
                _more_recv = false;
                _recv_part_index = 0;
                continue;
            }
            const int saved_errno = errno;
            const int close_rc = _message.close ();
            errno_assert (close_rc == 0);
            const int init_rc = _message.init ();
            errno_assert (init_rc == 0);
            if (saved_errno == EPROTO) {
                _recv_protocol_error_pending = true;
                errno = saved_errno;
                return true;
            }
            errno = saved_errno;
            return false;
        }

        ++skipped_non_matching;
        if (unlikely (skipped_non_matching >= xsub_non_matching_skip_budget))
            return false;
    }
}

int zlink::xsub_t::discard_filtered_message (msg_t *msg_, pipe_t *pipe_)
{
    pipe_t *const source_pipe =
      pipe_ && pipe_->retain_lifetime_ref () ? pipe_ : NULL;
    bool malformed = false;
    int rc = 0;
    while ((msg_->flags () & msg_t::more) != 0) {
        rc = _fq.recvpipe (msg_, &pipe_);
        if (rc != 0)
            break;
        unsigned char request_reply_kind = 0;
        uint64_t request_reply_sequence = 0;
        if (msg_->get_request_reply_metadata (
              &request_reply_kind, &request_reply_sequence))
            malformed = true;
    }

    if (malformed && source_pipe)
        source_pipe->terminate (false);
    if (source_pipe)
        source_pipe->release_lifetime_ref ();
    if (malformed) {
        errno = EPROTO;
        return -1;
    }
    return rc;
}

bool zlink::xsub_t::match (msg_t *msg_)
{
    if (!options.invert_matching && _has_empty_subscription.load (std::memory_order_acquire))
        return true;

    std::lock_guard<std::mutex> subscriptions_lock (_subscriptions_mu);
    if (!options.invert_matching && _has_empty_subscription.load (std::memory_order_relaxed))
        return true;

    const bool matching =
      _subscriptions.check (static_cast<unsigned char *> (msg_->data ()), msg_->size ());

    return matching ^ options.invert_matching;
}

void zlink::xsub_t::send_subscription (unsigned char *data_, size_t size_, void *arg_)
{
    pipe_t *pipe = static_cast<pipe_t *> (arg_);

    //  Create the subscription message.
    msg_t msg;
    const int rc = msg.init_subscribe (size_, data_);
    errno_assert (rc == 0);

    //  Send it to the pipe.
    const bool sent = pipe->write (&msg);
    //  If we reached the SNDHWM, and thus cannot send the subscription, drop
    //  the subscription message instead. This matches the behaviour of
    //  zlink_setsockopt(ZLINK_INTERNAL_OPT_SUBSCRIBE, ...), which also drops subscriptions
    //  when the SNDHWM is reached.
    if (!sent)
        msg.close ();
}

/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include <string.h>
#include <vector>

#include "utils/macros.hpp"
#include "sockets/pubsub/xsub.hpp"
#include "sockets/pubsub/xsub_dispatch_internal.hpp"
#include "utils/err.hpp"
#include "utils/env.hpp"

namespace
{
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

static void close_dispatch_frames (std::vector<zlink_msg_t> *frames_)
{
    if (!frames_)
        return;

    for (size_t i = 0; i < frames_->size (); ++i) {
        zlink::msg_t *msg = reinterpret_cast<zlink::msg_t *> (&(*frames_)[i]);
        const int rc = msg->close ();
        errno_assert (rc == 0);
    }
    frames_->clear ();
}

static void close_dispatch_frame (zlink_msg_t *frame_)
{
    if (!frame_)
        return;

    zlink::msg_t *msg = reinterpret_cast<zlink::msg_t *> (frame_);
    const int rc = msg->close ();
    errno_assert (rc == 0);
}

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
    _process_subscribe (false),
    _has_empty_subscription (false),
    _dispatch_active (false),
    _dispatch_callback (NULL),
    _dispatch_userdata (NULL),
    _dispatch_inflight (0),
    _dispatch_pending (false),
    _dispatch_draining (false),
    _dispatch_parts (),
    _socket_dispatch_drop_message (false),
    _delivery_ready_count (0)
{
    options.type = ZLINK_CORE_SOCKET_XSUB;
    refresh_auto_hwm_policy ();

    //  When socket is being closed down we don't want to wait till pending
    //  subscription commands are sent to the wire.
    options.linger.store (0);

    const int rc = _message.init ();
    errno_assert (rc == 0);
    _dispatch_parts.reserve (2);
    _socket_dispatch_parts.reserve (2);
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
    close_dispatch_frames (&_dispatch_parts);
    close_socket_msg_parts (&_socket_dispatch_parts);
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
    if (_dispatch_active.load (std::memory_order_acquire)) {
        pipe_->check_read ();
        _fq.deactivate (pipe_);
    }
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
    if (_dispatch_active.load (std::memory_order_acquire))
        (void) dispatch_ready_messages_serialized ();
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

int zlink::xsub_t::xsend (msg_t *msg_)
{
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
    //  If there's already a message prepared by a previous call to zlink_poll,
    //  return it straight ahead.
    if (_has_message) {
        const int rc = msg_->move (_message);
        errno_assert (rc == 0);
        _has_message = false;
        _more_recv = (msg_->flags () & msg_t::more) != 0;
        return 0;
    }

    //  Bound the number of non-matching messages skipped in one call so
    //  non-blocking recv semantics still return control under hostile traffic.
    unsigned int skipped_non_matching = 0;
    while (true) {
        //  Get a message using fair queueing algorithm.
        pipe_t *pipe = NULL;
        int rc = _fq.recvpipe (msg_, &pipe);

        //  If there's no message available, return immediately.
        //  The same when error occurs.
        if (rc != 0)
            return -1;

        //  Check whether the message matches at least one subscription.
        //  Non-initial parts of the message are passed
        const bool first_part = !_more_recv;
        if (_more_recv || !options.filter || match (msg_)) {
            _more_recv = (msg_->flags () & msg_t::more) != 0;
            if (first_part && recv_source_rid_capture_requested ())
                store_last_recv_source_rid (pipe);
            return 0;
        }

        //  Message doesn't match. Pop any remaining parts of the message
        //  from the pipe.
        while (msg_->flags () & msg_t::more) {
            rc = _fq.recvpipe (msg_, &pipe);
            errno_assert (rc == 0);
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
        int rc = _fq.recv (&_message);

        //  If there's no message available, return immediately.
        //  The same when error occurs.
        if (rc != 0) {
            errno_assert (errno == EAGAIN);
            return false;
        }

        //  Check whether the message matches at least one subscription.
        if (!options.filter || match (&_message)) {
            _has_message = true;
            return true;
        }

        //  Message doesn't match. Pop any remaining parts of the message
        //  from the pipe.
        while (_message.flags () & msg_t::more) {
            rc = _fq.recv (&_message);
            errno_assert (rc == 0);
        }

        ++skipped_non_matching;
        if (unlikely (skipped_non_matching >= xsub_non_matching_skip_budget))
            return false;
    }
}

void zlink::xsub_t::xdispatch_io ()
{
    if (_dispatch_active.load (std::memory_order_acquire))
        (void) dispatch_ready_messages_serialized ();
}

int zlink::xsub_t::dispatch_ready_messages_serialized ()
{
    _dispatch_pending.store (true, std::memory_order_release);

    bool expected = false;
    if (!_dispatch_draining.compare_exchange_strong (expected, true, std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
        return 0;
    }

    int rc = 0;
    for (;;) {
        _dispatch_pending.store (false, std::memory_order_release);
        rc = dispatch_ready_messages ();
        if (rc != 0)
            break;
        if (!_dispatch_pending.exchange (false, std::memory_order_acq_rel))
            break;
    }

    _dispatch_draining.store (false, std::memory_order_release);

    if (_dispatch_pending.exchange (false, std::memory_order_acq_rel)) {
        expected = false;
        if (_dispatch_draining.compare_exchange_strong (expected, true, std::memory_order_acq_rel,
                                                        std::memory_order_acquire)) {
            for (;;) {
                _dispatch_pending.store (false, std::memory_order_release);
                rc = dispatch_ready_messages ();
                if (rc != 0)
                    break;
                if (!_dispatch_pending.exchange (false, std::memory_order_acq_rel))
                    break;
            }
            _dispatch_draining.store (false, std::memory_order_release);
        }
    }

    return rc;
}

int zlink::xsub_t::dispatch_ready_messages ()
{
    while (_dispatch_active.load (std::memory_order_acquire)) {
        msg_t msg;
        const int init_rc = msg.init ();
        errno_assert (init_rc == 0);
        pipe_t *pipe = NULL;

        int rc = 0;
        while (true) {
            rc = _fq.recvpipe (&msg, &pipe);
            if (rc != 0)
                break;

            if (!options.filter || match (&msg))
                break;

            while (msg.flags () & msg_t::more) {
                rc = _fq.recvpipe (&msg, &pipe);
                errno_assert (rc == 0);
            }
        }
        if (rc != 0) {
            const int close_rc = msg.close ();
            errno_assert (close_rc == 0);
            if (errno == EAGAIN)
                return 0;
            return -1;
        }

        const int dispatch_rc = dispatch_message (&msg, pipe);
        if (dispatch_rc != 0)
            return dispatch_rc;
    }

    return 0;
}

int zlink::xsub_t::dispatch_message (msg_t *msg_, pipe_t *pipe_)
{
    if (!msg_) {
        errno = EINVAL;
        return -1;
    }

    close_dispatch_frames (&_dispatch_parts);
    while (true) {
        store_socket_msg_part (&_dispatch_parts, msg_);
        msg_t *stored_msg =
          reinterpret_cast<msg_t *> (&_dispatch_parts[_dispatch_parts.size () - 1]);

        if ((stored_msg->flags () & msg_t::more) == 0)
            break;

        const int next_init_rc = msg_->init ();
        errno_assert (next_init_rc == 0);
        if (_fq.recvpipe (msg_, &pipe_) != 0) {
            close_dispatch_frames (&_dispatch_parts);
            return -1;
        }
    }

    sub_io_handler_fn callback = _dispatch_callback.load (std::memory_order_acquire);
    if (!_dispatch_active.load (std::memory_order_acquire) || !callback) {
        close_dispatch_frames (&_dispatch_parts);
        return 0;
    }

    _dispatch_inflight.fetch_add (1, std::memory_order_acq_rel);
    callback = _dispatch_callback.load (std::memory_order_acquire);
    void *userdata = _dispatch_userdata.load (std::memory_order_acquire);
    if (!_dispatch_active.load (std::memory_order_acquire) || !callback) {
        close_dispatch_frames (&_dispatch_parts);
        notify_dispatch_stopped ();
        return 0;
    }

    const zlink_msg_t &topic = _dispatch_parts[0];
    const char *topic_data =
      static_cast<const char *> (zlink_msg_data (const_cast<zlink_msg_t *> (&topic)));
    const size_t topic_size = zlink_msg_size (const_cast<zlink_msg_t *> (&topic));
    const zlink_msg_t *payload =
      _dispatch_parts.size () > 1 ? &_dispatch_parts[1] : static_cast<zlink_msg_t *> (NULL);
    const size_t payload_count = _dispatch_parts.size () - 1;
    zlink_routing_id_t source_rid;
    resolve_socket_msg_source_rid (pipe_, &source_rid);

    {
        const zlink::xsub_dispatch_context_t dispatch_scope (this);
        callback (&source_rid, topic_data, topic_size, const_cast<zlink_msg_t *> (payload),
                  payload_count, userdata);
    }

    // The callback owns payload frames. The topic frame stays internal and is
    // released here after the handler returns.
    if (!_dispatch_parts.empty ())
        close_dispatch_frame (&_dispatch_parts[0]);
    _dispatch_parts.clear ();
    notify_dispatch_stopped ();
    return 0;
}

int zlink::xsub_t::xsocket_msg_dispatch (msg_t *msg_, pipe_t *pipe_)
{
    if (!socket_msg_dispatch_active ())
        return 0;

    const bool final_part = (msg_->flags () & msg_t::more) == 0;

    if (_socket_dispatch_parts.empty () && !_socket_dispatch_drop_message)
        _socket_dispatch_drop_message = !match (msg_);

    if (_socket_dispatch_drop_message) {
        if (final_part)
            _socket_dispatch_drop_message = false;
        return 1;
    }

    store_socket_msg_part (&_socket_dispatch_parts, msg_);
    if (!final_part)
        return 1;

    zlink_socket_msg_handler_fn handler = socket_msg_handler ();
    if (!handler) {
        close_socket_msg_parts (&_socket_dispatch_parts);
        return 1;
    }

    zlink_routing_id_t source_rid;
    resolve_socket_msg_source_rid (pipe_, &source_rid);
    invoke_socket_msg_handler (handler, &source_rid, &_socket_dispatch_parts[0],
                               _socket_dispatch_parts.size ());
    _socket_dispatch_parts.clear ();
    return 1;
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

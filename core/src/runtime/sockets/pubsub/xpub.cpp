/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include <string.h>

#include "sockets/pubsub/xpub.hpp"
#include "core/pipe.hpp"
#include "utils/err.hpp"
#include "core/msg.hpp"
#include "utils/macros.hpp"
#include "utils/generic_mtrie_impl.hpp"

zlink::xpub_t::xpub_t (class ctx_t *parent_, uint32_t tid_, int sid_) :
    socket_base_t (parent_, tid_, sid_),
    _verbose_subs (false),
    _verbose_unsubs (false),
    _more_send (false),
    _more_recv (false),
    _process_subscribe (false),
    _lossy (true),
    _send_all_data (false),
    _manual (false),
    _send_last_pipe (false),
    _pending_pipes (),
    _welcome_msg (),
    _dispatch_active (false),
    _dispatch_inflight (0),
    _delivery_ready_peer_count (0)
{
    _last_pipe = NULL;
    options.type = ZLINK_CORE_SOCKET_XPUB;
    refresh_auto_hwm_policy ();
    _welcome_msg.init ();
}

zlink::xpub_t::~xpub_t ()
{
    _welcome_msg.close ();
}

bool zlink::xpub_t::compute_delivery_ready_state () const
{
    return compute_delivery_ready_count () > 0;
}

void zlink::xpub_t::collect_ready_pipe (pipe_t *pipe_, std::set<pipe_t *> *out_)
{
    if (pipe_ && out_)
        out_->insert (pipe_);
}

uint32_t zlink::xpub_t::compute_delivery_ready_count () const
{
    if (_subscriptions.num_prefixes () == 0)
        return 0;

    std::set<pipe_t *> ready_pipes;
    _subscriptions.visit_values (&collect_ready_pipe, &ready_pipes);
    return static_cast<uint32_t> (ready_pipes.size ());
}

void zlink::xpub_t::refresh_delivery_ready_state (const endpoint_uri_pair_t &endpoint_uri_pair_)
{
    LIBZLINK_UNUSED (endpoint_uri_pair_);
    const uint32_t ready_count = compute_delivery_ready_count ();
    _delivery_ready_peer_count.store (ready_count, std::memory_order_release);
}

uint32_t zlink::xpub_t::monitor_ready_count () const
{
    return _delivery_ready_peer_count.load (std::memory_order_acquire);
}

void zlink::xpub_t::xattach_pipe (pipe_t *pipe_, bool subscribe_to_all_, bool locally_initiated_)
{
    LIBZLINK_UNUSED (locally_initiated_);

    zlink_assert (pipe_);
    _dist.attach (pipe_);

    //  If subscribe_to_all_ is specified, the caller would like to subscribe
    //  to all data on this pipe, implicitly.
    if (subscribe_to_all_) {
        _subscriptions.add (NULL, 0, pipe_);
        refresh_delivery_ready_state (pipe_->get_endpoint_pair ());
    }

    // if welcome message exists, send a copy of it
    if (_welcome_msg.size () > 0) {
        msg_t copy;
        copy.init ();
        const int rc = copy.copy (_welcome_msg);
        errno_assert (rc == 0);
        const bool ok = pipe_->write_and_flush (&copy);
        zlink_assert (ok);
    }

    //  The pipe is active when attached. Let's read the subscriptions from
    //  it, if any.
    xread_activated (pipe_);
}

void zlink::xpub_t::xread_activated (pipe_t *pipe_)
{
    //  There are some subscriptions waiting. Let's process them.
    msg_t msg;
    while (pipe_->read (&msg)) {
        unsigned char *msg_data = static_cast<unsigned char *> (msg.data ()), *data = NULL;
        size_t size = 0;
        bool subscribe = false;
        bool is_subscribe_or_cancel = false;
        bool notify = false;

        const bool first_part = !_more_recv;
        _more_recv = (msg.flags () & msg_t::more) != 0;

        if (first_part || _process_subscribe) {
            //  Apply the subscription to the trie
            if (msg.is_subscribe () || msg.is_cancel ()) {
                data = static_cast<unsigned char *> (msg.command_body ());
                size = msg.command_body_size ();
                if (data || size) {
                    subscribe = msg.is_subscribe ();
                    is_subscribe_or_cancel = true;
                }
            } else if (msg.size () > 0 && (*msg_data == 0 || *msg_data == 1)) {
                data = msg_data + 1;
                size = msg.size () - 1;
                subscribe = *msg_data == 1;
                is_subscribe_or_cancel = true;
            }
        }

        if (first_part)
            _process_subscribe = is_subscribe_or_cancel;

        if (is_subscribe_or_cancel) {
            if (_manual) {
                // Store manual subscription to use on termination
                if (!subscribe)
                    _manual_subscriptions.rm (data, size, pipe_);
                else
                    _manual_subscriptions.add (data, size, pipe_);
            } else {
                if (!subscribe) {
                    const mtrie_t::rm_result rm_result = _subscriptions.rm (data, size, pipe_);
                    notify = rm_result != mtrie_t::values_remain || _verbose_unsubs;
                } else {
                    const bool first_added = _subscriptions.add (data, size, pipe_);
                    notify = first_added || _verbose_subs;
                }
            }

            //  If the request was a new subscription, or the subscription
            //  was removed, or verbose mode or manual mode are enabled, store it
            //  so that it can be passed to the user on next recv call.
            if (_manual || (options.type == ZLINK_CORE_SOCKET_XPUB && notify)) {
                //  Translate ZMTP 3.1 sub/cancel commands into the public XPUB
                //  notification shape. Inproc does not carry the command string,
                //  so build the notification frame explicitly.
                blob_t notification (size + 1);
                if (subscribe)
                    *notification.data () = 1;
                else
                    *notification.data () = 0;
                memcpy (notification.data () + 1, data, size);

                _pending_data.push_back (ZLINK_MOVE (notification));
                _pending_flags.push_back (0);
                _pending_pipes.push_back (pipe_);
            }
        } else if (options.type != ZLINK_CORE_SOCKET_PUB) {
            //  Process user message coming upstream from xsub socket,
            //  but not if the type is PUB, which never processes user
            //  messages
            _pending_data.push_back (blob_t (msg_data, msg.size ()));
            _pending_flags.push_back (msg.flags ());
            _pending_pipes.push_back (pipe_);
        }
        msg.close ();
    }

    if (pipe_)
        refresh_delivery_ready_state (pipe_->get_endpoint_pair ());

    if (_dispatch_active.load (std::memory_order_acquire))
        dispatch_ready_messages ();
}

void zlink::xpub_t::xwrite_activated (pipe_t *pipe_)
{
    _dist.activated (pipe_);
    if (pipe_)
        refresh_delivery_ready_state (pipe_->get_endpoint_pair ());
}

int zlink::xpub_t::xsetsockopt (int option_, const void *optval_, size_t optvallen_)
{
    if (option_ == ZLINK_INTERNAL_OPT_XPUB_VERBOSE || option_ == ZLINK_INTERNAL_OPT_XPUB_VERBOSER
        || option_ == ZLINK_INTERNAL_OPT_XPUB_MANUAL_LAST_VALUE
        || option_ == ZLINK_INTERNAL_OPT_XPUB_NODROP || option_ == ZLINK_INTERNAL_OPT_XPUB_MANUAL) {
        if (optvallen_ != sizeof (int) || *static_cast<const int *> (optval_) < 0) {
            errno = EINVAL;
            return -1;
        }
        if (option_ == ZLINK_INTERNAL_OPT_XPUB_VERBOSE) {
            _verbose_subs = (*static_cast<const int *> (optval_) != 0);
            _verbose_unsubs = false;
        } else if (option_ == ZLINK_INTERNAL_OPT_XPUB_VERBOSER) {
            _verbose_subs = (*static_cast<const int *> (optval_) != 0);
            _verbose_unsubs = _verbose_subs;
        } else if (option_ == ZLINK_INTERNAL_OPT_XPUB_MANUAL_LAST_VALUE) {
            _manual = (*static_cast<const int *> (optval_) != 0);
            _send_last_pipe = _manual;
        } else if (option_ == ZLINK_INTERNAL_OPT_XPUB_NODROP)
            _lossy = (*static_cast<const int *> (optval_) == 0);
        else if (option_ == ZLINK_INTERNAL_OPT_XPUB_MANUAL)
            _manual = (*static_cast<const int *> (optval_) != 0);
    } else if (option_ == send_all_data_option) {
        if (optvallen_ != sizeof (int) || *static_cast<const int *> (optval_) < 0) {
            errno = EINVAL;
            return -1;
        }
        _send_all_data = (*static_cast<const int *> (optval_) != 0);
    } else if (option_ == ZLINK_INTERNAL_OPT_SUBSCRIBE && _manual) {
        if (_last_pipe != NULL) {
            _subscriptions.add ((unsigned char *) optval_, optvallen_, _last_pipe);
        }
    } else if (option_ == ZLINK_INTERNAL_OPT_UNSUBSCRIBE && _manual) {
        if (_last_pipe != NULL) {
            _subscriptions.rm ((unsigned char *) optval_, optvallen_, _last_pipe);
        }
    } else if (option_ == ZLINK_INTERNAL_OPT_XPUB_WELCOME_MSG) {
        _welcome_msg.close ();

        if (optvallen_ > 0) {
            const int rc = _welcome_msg.init_size (optvallen_);
            errno_assert (rc == 0);

            unsigned char *data = static_cast<unsigned char *> (_welcome_msg.data ());
            memcpy (data, optval_, optvallen_);
        } else
            _welcome_msg.init ();
    } else {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int zlink::xpub_t::xgetsockopt (int option_, void *optval_, size_t *optvallen_)
{
    if (option_ == ZLINK_INTERNAL_OPT_TOPICS_COUNT) {
        // make sure to use a multi-thread safe function to avoid race conditions with I/O threads
        // where subscriptions are processed:
        return do_getsockopt<int> (optval_, optvallen_, (int) _subscriptions.num_prefixes ());
    }

    if (option_ == ZLINK_INTERNAL_OPT_XPUB_VERBOSE)
        return do_getsockopt<int> (optval_, optvallen_, _verbose_subs ? 1 : 0);

    if (option_ == ZLINK_INTERNAL_OPT_XPUB_VERBOSER)
        return do_getsockopt<int> (optval_, optvallen_, (_verbose_subs && _verbose_unsubs) ? 1 : 0);

    if (option_ == ZLINK_INTERNAL_OPT_XPUB_MANUAL)
        return do_getsockopt<int> (optval_, optvallen_, _manual ? 1 : 0);

    if (option_ == ZLINK_INTERNAL_OPT_XPUB_MANUAL_LAST_VALUE)
        return do_getsockopt<int> (optval_, optvallen_, (_manual && _send_last_pipe) ? 1 : 0);

    if (option_ == ZLINK_INTERNAL_OPT_XPUB_NODROP)
        return do_getsockopt<int> (optval_, optvallen_, _lossy ? 0 : 1);

    if (option_ == ZLINK_INTERNAL_OPT_XPUB_WELCOME_MSG) {
        return do_getsockopt (optval_, optvallen_, _welcome_msg.data (), _welcome_msg.size ());
    }

    // room for future options here

    errno = EINVAL;
    return -1;
}

static void stub (zlink::mtrie_t::prefix_t data_, size_t size_, void *arg_)
{
    LIBZLINK_UNUSED (data_);
    LIBZLINK_UNUSED (size_);
    LIBZLINK_UNUSED (arg_);
}

void zlink::xpub_t::xpipe_terminated (pipe_t *pipe_)
{
    const endpoint_uri_pair_t endpoint_pair =
      pipe_ ? pipe_->get_endpoint_pair () : endpoint_uri_pair_t ();
    if (_manual) {
        //  Remove the pipe from the trie and send corresponding manual
        //  unsubscriptions upstream.
        _manual_subscriptions.rm (pipe_, send_unsubscription, this, false);
        //  Remove pipe without actually sending the message as it was taken
        //  care of by the manual call above. subscriptions is the real mtrie,
        //  so the pipe must be removed from there or it will be left over.
        _subscriptions.rm (pipe_, stub, static_cast<void *> (NULL), false);

        // In case the pipe is currently set as last we must clear it to prevent
        // subscriptions from being re-added.
        if (pipe_ == _last_pipe) {
            _last_pipe = NULL;
        }
    } else {
        //  Remove the pipe from the trie. If there are topics that nobody
        //  is interested in anymore, send corresponding unsubscriptions
        //  upstream.
        _subscriptions.rm (pipe_, send_unsubscription, this, !_verbose_unsubs);
    }

    _dist.pipe_terminated (pipe_);
    refresh_delivery_ready_state (endpoint_pair);
}

void zlink::xpub_t::mark_as_matching (pipe_t *pipe_, xpub_t *self_)
{
    self_->_dist.match (pipe_);
}

void zlink::xpub_t::mark_last_pipe_as_matching (pipe_t *pipe_, xpub_t *self_)
{
    if (self_->_last_pipe == pipe_)
        self_->_dist.match (pipe_);
}

int zlink::xpub_t::xsend (msg_t *msg_)
{
    const bool msg_more = (msg_->flags () & msg_t::more) != 0;

    if (_send_all_data && !_manual && !options.invert_matching) {
        const pipe_message_admission_t admission = _dist.check_hwm (msg_);
        //  Lossy delivery forgives a peer that cannot take this message: a full
        //  queue and a pipe that left the active state are both "this one peer
        //  misses it", not a reason to fail the send for every other peer.
        //  dist_t::distribute already drops the copy for a pipe whose write
        //  fails, so admitting here loses nothing else.
        const bool admitted =
          admission == pipe_message_admission_ready
          || (_lossy
              && (admission == pipe_message_admission_hwm_full
                  || admission == pipe_message_admission_inactive));
        int rc = -1;
        if (admitted) {
            if (_dist.send_to_all (msg_) == 0) {
                _more_send = msg_more;
                rc = 0;
            }
        } else
            errno = admission == pipe_message_admission_too_large ? EMSGSIZE : EAGAIN;
        return rc;
    }

    //  For the first part of multi-part message, find the matching pipes.
    if (!_more_send) {
        // Ensure nothing from previous failed attempt to send is left matched
        _dist.unmatch ();

        if (unlikely (_manual && _last_pipe && _send_last_pipe)) {
            _subscriptions.match (static_cast<unsigned char *> (msg_->data ()), msg_->size (),
                                  mark_last_pipe_as_matching, this);
            _last_pipe = NULL;
        } else {
            _subscriptions.match (static_cast<unsigned char *> (msg_->data ()), msg_->size (),
                                  mark_as_matching, this);
        }
        // If inverted matching is used, reverse the selection now
        if (options.invert_matching) {
            _dist.reverse_match ();
        }
    }

    const pipe_message_admission_t admission = _dist.check_hwm (msg_);
    //  Same rule as the send-all path above.
    const bool admitted =
      admission == pipe_message_admission_ready
      || (_lossy
          && (admission == pipe_message_admission_hwm_full
              || admission == pipe_message_admission_inactive));
    int rc = -1; //  Assume we fail
    if (admitted) {
        if (_dist.send_to_matching (msg_) == 0) {
            //  If we are at the end of multi-part message we can mark
            //  all the pipes as non-matching.
            if (!msg_more)
                _dist.unmatch ();
            _more_send = msg_more;
            rc = 0; //  Yay, sent successfully
        }
    } else
        errno = admission == pipe_message_admission_too_large ? EMSGSIZE : EAGAIN;
    return rc;
}

int zlink::xpub_t::xrollback ()
{
    _dist.rollback ();
    _more_send = false;
    return 0;
}

bool zlink::xpub_t::xhas_out ()
{
    return _dist.has_out ();
}

int zlink::xpub_t::xrecv (msg_t *msg_)
{
    //  If there is at least one
    if (_pending_data.empty ()) {
        errno = EAGAIN;
        return -1;
    }

    // User is reading a message, set last_pipe and remove it from the deque
    if (!_pending_pipes.empty ()) {
        _last_pipe = _pending_pipes.front ();
        _pending_pipes.pop_front ();

        // If the distributor doesn't know about this pipe it must have already
        // been terminated and thus we can't allow manual subscriptions.
        if (_last_pipe != NULL && !_dist.has_pipe (_last_pipe)) {
            _last_pipe = NULL;
        }
    }
    if (_last_pipe != NULL)
        store_last_recv_source_rid (_last_pipe);
    else
        clear_last_recv_source_rid ();

    int rc = msg_->close ();
    errno_assert (rc == 0);
    rc = msg_->init_size (_pending_data.front ().size ());
    errno_assert (rc == 0);
    memcpy (msg_->data (), _pending_data.front ().data (), _pending_data.front ().size ());

    msg_->set_flags (_pending_flags.front ());
    _pending_data.pop_front ();
    _pending_flags.pop_front ();
    return 0;
}

bool zlink::xpub_t::xhas_in ()
{
    return !_pending_data.empty ();
}

void zlink::xpub_t::xdispatch_io ()
{
    if (!_dispatch_active.load (std::memory_order_acquire))
        return;

    while (_dispatch_active.load (std::memory_order_acquire)) {
        const int rc = dispatch_ready_messages ();
        if (rc != 0)
            return;
        if (!xhas_in ())
            return;
    }
}

int zlink::xpub_t::xpub_dispatch_start ()
{
    io_thread_t *io_thread = choose_io_thread (options.affinity);
    if (!io_thread) {
        errno = EAGAIN;
        return -1;
    }

    std::lock_guard<std::mutex> lk (_dispatch_control_mu);
    if (_dispatch_active.load (std::memory_order_acquire))
        return 0;

    _dispatch_active.store (true, std::memory_order_release);
    if (start_async_mailbox_processing (io_thread) != 0) {
        _dispatch_active.store (false, std::memory_order_release);
        return -1;
    }
    return 0;
}

bool zlink::xpub_t::xpub_dispatch_active () const
{
    return _dispatch_active.load (std::memory_order_acquire);
}

int zlink::xpub_t::dispatch_ready_messages ()
{
    while (_dispatch_active.load (std::memory_order_acquire) && xhas_in ()) {
        msg_t msg;
        const int init_rc = msg.init ();
        errno_assert (init_rc == 0);

        const int rc = xrecv (&msg);
        if (rc != 0) {
            const int close_rc = msg.close ();
            errno_assert (close_rc == 0);
            return -1;
        }

        const int dispatch_rc = dispatch_message (&msg);
        const int close_rc = msg.close ();
        errno_assert (close_rc == 0);
        if (dispatch_rc != 0)
            return dispatch_rc;
    }

    return 0;
}

int zlink::xpub_t::dispatch_message (zlink::msg_t *msg_)
{
    LIBZLINK_UNUSED (msg_);
    return 0;
}

void zlink::xpub_t::notify_dispatch_stopped ()
{
    const uint32_t remaining = _dispatch_inflight.fetch_sub (1, std::memory_order_acq_rel) - 1;
    if (remaining == 0) {
        std::lock_guard<std::mutex> lk (_dispatch_inflight_mu);
        _dispatch_inflight_cv.notify_all ();
    }
}

void zlink::xpub_t::send_unsubscription (zlink::mtrie_t::prefix_t data_,
                                         size_t size_,
                                         xpub_t *self_)
{
    if (self_->options.type != ZLINK_CORE_SOCKET_PUB) {
        //  Place the unsubscription to the queue of pending (un)subscriptions
        //  to be retrieved by the user later on.
        blob_t unsub (size_ + 1);
        *unsub.data () = 0;
        if (size_ > 0)
            memcpy (unsub.data () + 1, data_, size_);
        self_->_pending_data.ZLINK_PUSH_OR_EMPLACE_BACK (ZLINK_MOVE (unsub));
        self_->_pending_flags.push_back (0);

        self_->_last_pipe = NULL;
        self_->_pending_pipes.push_back (NULL);
    }
}

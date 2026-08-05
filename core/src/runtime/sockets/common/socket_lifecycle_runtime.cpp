/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "sockets/common/socket_runtime.hpp"

#include "core/io_thread.hpp"
#include "core/mailbox.hpp"
#include "sockets/common/socket_base.hpp"
#include "utils/err.hpp"
#include "utils/macros.hpp"

namespace
{
const uint32_t public_api_closing_bit = 0x80000000u;
const uint32_t public_api_sync_bit = 0x40000000u;
const uint32_t public_api_inflight_mask = ~(public_api_closing_bit | public_api_sync_bit);
}

bool zlink::socket_lifecycle_coordinator_t::enter_public_api ()
{
    const uint32_t old = public_api_state.fetch_add (1, std::memory_order_acq_rel);
    if ((old & public_api_closing_bit) == 0)
        return true;

    const uint32_t reverted = public_api_state.fetch_sub (1, std::memory_order_acq_rel);
    zlink_assert ((reverted & public_api_inflight_mask) > 0);
    errno = ESHUTDOWN;
    return false;
}

bool zlink::socket_lifecycle_coordinator_t::enter_public_api_and_lock_sync ()
{
    uint32_t expected = 0;
    if (public_api_state.compare_exchange_strong (expected, 1u | public_api_sync_bit,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
        return true;
    }

    if (!enter_public_api ())
        return false;

    lock_public_api_sync ();
    return true;
}

void zlink::socket_lifecycle_coordinator_t::leave_public_api ()
{
    const uint32_t old = public_api_state.fetch_sub (1, std::memory_order_acq_rel);
    zlink_assert ((old & public_api_inflight_mask) > 0);
}

bool zlink::socket_lifecycle_coordinator_t::enter_callback_api ()
{
    if (!enter_public_api ())
        return false;

    callback_api_depth.fetch_add (1, std::memory_order_acq_rel);
    return true;
}

bool zlink::socket_lifecycle_coordinator_t::leave_callback_api ()
{
    const uint32_t depth = callback_api_depth.fetch_sub (1, std::memory_order_acq_rel) - 1;
    leave_public_api ();
    return depth == 0 && close_deferred.load (std::memory_order_acquire)
           && public_close_requested ();
}

bool zlink::socket_lifecycle_coordinator_t::begin_close_or_fail_busy (bool from_self_callback_)
{
    uint32_t old = public_api_state.load (std::memory_order_acquire);
    while (true) {
        if ((old & public_api_closing_bit) != 0) {
            errno = EALREADY;
            return false;
        }

        const uint32_t inflight = old & public_api_inflight_mask;
        if (!from_self_callback_) {
            if (inflight != 0) {
                errno = EBUSY;
                return false;
            }
        } else {
            // A callback may defer only its own close. Another callback or
            // public API on any thread keeps the socket busy.
            const uint32_t callbacks =
              callback_api_depth.load (std::memory_order_acquire);
            if (callbacks != 1 || inflight != 1) {
                errno = EBUSY;
                return false;
            }
        }

        const uint32_t desired = old | public_api_closing_bit;
        if (public_api_state.compare_exchange_weak (old, desired, std::memory_order_acq_rel,
                                                    std::memory_order_acquire)) {
            if (from_self_callback_)
                close_deferred.store (true, std::memory_order_release);
            return true;
        }
    }
}

bool zlink::socket_lifecycle_coordinator_t::public_close_requested () const
{
    return (public_api_state.load (std::memory_order_acquire) & public_api_closing_bit) != 0;
}

bool zlink::socket_lifecycle_coordinator_t::public_api_sync_held () const
{
    return (public_api_state.load (std::memory_order_acquire) & public_api_sync_bit) != 0;
}

void zlink::socket_lifecycle_coordinator_t::lock_public_api_sync ()
{
    uint32_t old = public_api_state.load (std::memory_order_acquire);
    while (true) {
        if ((old & public_api_sync_bit) == 0) {
            const uint32_t desired = old | public_api_sync_bit;
            if (public_api_state.compare_exchange_weak (old, desired, std::memory_order_acquire,
                                                        std::memory_order_acquire)) {
                return;
            }
            continue;
        }

        old = public_api_state.load (std::memory_order_acquire);
    }
}

void zlink::socket_lifecycle_coordinator_t::unlock_public_api_sync ()
{
    const uint32_t old =
      public_api_state.fetch_and (~public_api_sync_bit, std::memory_order_release);
    zlink_assert ((old & public_api_sync_bit) != 0);
}

void zlink::socket_lifecycle_coordinator_t::unlock_public_api_sync_and_leave ()
{
    const uint32_t old =
      public_api_state.fetch_sub (public_api_sync_bit | 1u, std::memory_order_acq_rel);
    zlink_assert ((old & public_api_sync_bit) != 0);
    zlink_assert ((old & public_api_inflight_mask) > 0);
}

zlink::socket_callback_scope_t::socket_callback_scope_t (socket_base_t *socket_) :
    _socket (socket_),
    _coordinator (socket_ ? &socket_->lifecycle_coordinator () : NULL),
    _entered (_coordinator && _coordinator->enter_callback_api ())
{
}

zlink::socket_callback_scope_t::~socket_callback_scope_t ()
{
    if (!_entered)
        return;

    if (!_coordinator->leave_callback_api () || !_socket)
        return;

    if (socket_base_t::current_async_mailbox_dispatch_socket () == _socket) {
        _socket->defer_close_handoff_from_async_owner ();
        return;
    }

    _socket->finish_close_handoff ();
}

zlink::socket_public_send_scope_t::socket_public_send_scope_t (
  socket_lifecycle_coordinator_t &coordinator_, bool needs_sync_) :
    _coordinator (&coordinator_), _entered (false), _needs_sync (needs_sync_), _sync_locked (false)
{
    if (_needs_sync) {
        _entered = _coordinator->enter_public_api_and_lock_sync ();
        _sync_locked = _entered;
        return;
    }

    _entered = _coordinator->enter_public_api ();
}

zlink::socket_public_send_scope_t::~socket_public_send_scope_t ()
{
    if (!_entered)
        return;

    if (_sync_locked)
        _coordinator->unlock_public_api_sync_and_leave ();
    else
        _coordinator->leave_public_api ();
}

bool zlink::socket_public_send_scope_t::should_hold_sync_during_retry (
  bool send_ready_handler_active_) const
{
    return _sync_locked && !send_ready_handler_active_;
}

void zlink::socket_public_send_scope_t::release_sync_for_retry ()
{
    unlock_sync ();
}

void zlink::socket_public_send_scope_t::reacquire_sync_after_retry ()
{
    relock_sync ();
}

void zlink::socket_public_send_scope_t::unlock_sync ()
{
    if (!_entered || !_needs_sync || !_sync_locked)
        return;

    _coordinator->unlock_public_api_sync ();
    _sync_locked = false;
}

void zlink::socket_public_send_scope_t::relock_sync ()
{
    if (!_entered || !_needs_sync || _sync_locked)
        return;

    _coordinator->lock_public_api_sync ();
    _sync_locked = true;
}

int zlink::socket_lifecycle_coordinator_t::start_async_mailbox_processing (
  mailbox_t *mailbox_,
  io_thread_t *io_thread_,
  mailbox_t::mailbox_handler_t handler_,
  void *handler_arg_,
  mailbox_t::mailbox_pre_post_t pre_post_)
{
    if (!mailbox_ || !io_thread_) {
        errno = EINVAL;
        return -1;
    }

    async_mailbox_active.store (true, std::memory_order_release);
    async_processing_started.store (false, std::memory_order_release);
    mailbox_->set_io_context (&io_thread_->get_io_context (), handler_, handler_arg_, pre_post_);
    mailbox_->schedule_if_needed ();
    return 0;
}

void zlink::socket_lifecycle_coordinator_t::mark_async_processing_started ()
{
    async_processing_started.store (true, std::memory_order_release);
    scoped_lock_t lock (async_done_mu);
    async_done_cv.broadcast ();
}

void zlink::socket_lifecycle_coordinator_t::wait_async_started (int timeout_ms_)
{
    if (async_processing_started.load (std::memory_order_acquire))
        return;

    scoped_lock_t lock (async_done_mu);
    while (!async_processing_started.load (std::memory_order_acquire)) {
        const int rc = async_done_cv.wait (&async_done_mu, timeout_ms_ > 0 ? timeout_ms_ : 2000);
        if (rc != 0)
            break;
    }
}

void zlink::socket_lifecycle_coordinator_t::stop_async_mailbox_processing (mailbox_t *mailbox_)
{
    async_mailbox_active.store (false, std::memory_order_release);
    async_processing_done.store (false, std::memory_order_release);
    async_quiesce_pending.store (true, std::memory_order_release);
    if (mailbox_)
        mailbox_->schedule_if_needed ();
}

void zlink::socket_lifecycle_coordinator_t::mark_async_processing_stopped (mailbox_t *mailbox_)
{
    if (mailbox_)
        mailbox_->set_io_context (NULL, NULL, NULL, NULL);

    if (async_quiesce_pending.load (std::memory_order_acquire)) {
        async_quiesce_pending.store (false, std::memory_order_release);
        async_processing_done.store (true, std::memory_order_release);
        scoped_lock_t lock (async_done_mu);
        async_done_cv.broadcast ();
    }
}

void zlink::socket_lifecycle_coordinator_t::wait_async_quiesced (int timeout_ms_)
{
    if (async_processing_done.load (std::memory_order_acquire))
        return;

    scoped_lock_t lock (async_done_mu);
    const int wait_timeout_ms = timeout_ms_ < 0 ? -1 : timeout_ms_ > 0 ? timeout_ms_ : 2000;
    while (!async_processing_done.load (std::memory_order_acquire)) {
        const int rc = async_done_cv.wait (&async_done_mu, wait_timeout_ms);
        if (rc != 0)
            break;
    }
}

bool zlink::socket_lifecycle_coordinator_t::is_async_mailbox_active () const
{
    return async_mailbox_active.load (std::memory_order_acquire);
}

bool zlink::socket_lifecycle_coordinator_t::is_async_quiesce_pending () const
{
    return async_quiesce_pending.load (std::memory_order_acquire);
}

void zlink::socket_lifecycle_coordinator_t::complete_deferred_close_handoff (mailbox_t *mailbox_,
                                                                             int timeout_ms_)
{
    clear_deferred_close ();

    if (is_async_mailbox_active ()) {
        stop_async_mailbox_processing (mailbox_);
        wait_async_quiesced (timeout_ms_);
    } else if (is_async_quiesce_pending ()) {
        wait_async_quiesced (timeout_ms_);
    }

    if (mailbox_)
        mailbox_->clear_signalers ();
}

void zlink::socket_lifecycle_coordinator_t::clear_deferred_close ()
{
    close_deferred.store (false, std::memory_order_release);
}

bool zlink::socket_lifecycle_coordinator_t::take_deferred_close ()
{
    return close_deferred.exchange (false, std::memory_order_acq_rel);
}

void zlink::socket_lifecycle_coordinator_t::set_monitor_async_mailbox_owned (bool owned_)
{
    monitor_async_mailbox_owned = owned_;
}

bool zlink::socket_lifecycle_coordinator_t::is_monitor_async_mailbox_owned () const
{
    return monitor_async_mailbox_owned;
}

void zlink::socket_lifecycle_coordinator_t::mark_destroy_pending ()
{
    destroy_pending = true;
}

void zlink::socket_lifecycle_coordinator_t::clear_destroy_pending ()
{
    destroy_pending = false;
}

bool zlink::socket_lifecycle_coordinator_t::is_destroy_pending () const
{
    return destroy_pending;
}

void zlink::socket_lifecycle_coordinator_t::set_reaper_poller (poller_t *poller_)
{
    reaper_poller_value = poller_;
}

zlink::poller_t *zlink::socket_lifecycle_coordinator_t::reaper_poller () const
{
    return reaper_poller_value;
}

void zlink::socket_lifecycle_coordinator_t::mark_destroyed ()
{
    destroyed = true;
}

bool zlink::socket_lifecycle_coordinator_t::is_destroyed () const
{
    return destroyed;
}

int zlink::socket_lifecycle_coordinator_t::mailbox_refcount ()
{
    return mailbox_refcnt.add (0);
}

void zlink::socket_lifecycle_coordinator_t::inc_mailbox_ref ()
{
    mailbox_refcnt.add (1);
}

bool zlink::socket_lifecycle_coordinator_t::dec_mailbox_ref ()
{
    return mailbox_refcnt.sub (1) != 0;
}

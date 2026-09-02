/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "sockets/common/socket_runtime.hpp"

#include "core/io_thread.hpp"
#include "core/mailbox.hpp"
#include "sockets/common/socket_base.hpp"
#include "utils/err.hpp"
#include "utils/macros.hpp"

#include <chrono>
#include <thread>

namespace
{
// Keep send admission in the lifecycle word so a successful public send
// needs one atomic acquire/release pair. The low word remains the lifecycle
// in-flight count; the high word owns close, public synchronization, one
// incremental multipart marker, and the complete-record admission count.
const uint64_t public_api_closing_bit = UINT64_C (1) << 63;
const uint64_t public_api_sync_bit = UINT64_C (1) << 62;
const uint64_t public_api_multipart_bit = UINT64_C (1) << 61;
const uint64_t public_api_complete_unit = UINT64_C (1) << 32;
const uint64_t public_api_complete_mask =
  ((UINT64_C (1) << 29) - 1) * public_api_complete_unit;
const uint64_t public_api_inflight_mask = UINT64_C (0xffffffff);
const uint32_t mailbox_ref_sealed_bit = UINT32_C (1) << 31;
const uint32_t mailbox_ref_count_mask = mailbox_ref_sealed_bit - 1;

//  The sync bit is held for the whole body of a public API call, which can be
//  arbitrarily long (a blocking recv, an endpoint teardown). A contended
//  waiter therefore cannot assume the holder releases within a few hundred
//  cycles, so an unbounded CAS spin burns a whole core - observed as a thread
//  pinned at 100% CPU during shutdown. Spin briefly for the common short hold,
//  then yield, then sleep so a long hold costs no CPU.
const unsigned int public_api_sync_spin_limit = 64;
const unsigned int public_api_sync_yield_limit = 1024;

void public_api_sync_backoff (unsigned int attempt_)
{
    if (attempt_ < public_api_sync_spin_limit)
        return;
    if (attempt_ < public_api_sync_yield_limit) {
        std::this_thread::yield ();
        return;
    }
    std::this_thread::sleep_for (std::chrono::microseconds (100));
}

}

thread_local zlink::socket_lifecycle_coordinator_t *
  zlink::socket_lifecycle_coordinator_t::_current_thread_public_api_sync_owner =
    NULL;

bool zlink::socket_lifecycle_coordinator_t::enter_public_api ()
{
    const uint64_t old = public_api_state.fetch_add (1, std::memory_order_acq_rel);
    zlink_assert ((old & public_api_inflight_mask) != public_api_inflight_mask);
    if ((old & public_api_closing_bit) == 0)
        return true;

    const uint64_t reverted = public_api_state.fetch_sub (1, std::memory_order_acq_rel);
    zlink_assert ((reverted & public_api_inflight_mask) > 0);
    errno = ESHUTDOWN;
    return false;
}

bool zlink::socket_lifecycle_coordinator_t::enter_public_api_and_lock_sync ()
{
    uint64_t expected = 0;
    if (public_api_state.compare_exchange_strong (expected, 1u | public_api_sync_bit,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
        mark_public_api_sync_owned ();
        return true;
    }

    if (!enter_public_api ())
        return false;

    lock_public_api_sync ();
    return true;
}

void zlink::socket_lifecycle_coordinator_t::leave_public_api ()
{
    const uint64_t old = public_api_state.fetch_sub (1, std::memory_order_acq_rel);
    zlink_assert ((old & public_api_inflight_mask) > 0);
}

bool zlink::socket_lifecycle_coordinator_t::enter_public_send (
  bool needs_sync_,
  bool multipart_sequence_,
  bool *sync_locked_out_,
  bool *multipart_active_out_)
{
    if (sync_locked_out_)
        *sync_locked_out_ = false;
    if (multipart_active_out_)
        *multipart_active_out_ = false;

    uint64_t old = public_api_state.load (std::memory_order_acquire);
    while (true) {
        if ((old & public_api_closing_bit) != 0) {
            errno = ESHUTDOWN;
            return false;
        }
        if ((old & public_api_inflight_mask) == public_api_inflight_mask) {
            errno = EBUSY;
            return false;
        }

        if (multipart_sequence_) {
            if ((old & (public_api_multipart_bit | public_api_complete_mask)) != 0) {
                errno = EINVAL;
                return false;
            }
        } else {
            if ((old & public_api_multipart_bit) != 0) {
                if (multipart_active_out_)
                    *multipart_active_out_ = true;
                errno = EINVAL;
                return false;
            }
            if ((old & public_api_complete_mask) == public_api_complete_mask) {
                errno = EBUSY;
                return false;
            }
        }

        uint64_t desired = old + 1;
        desired += multipart_sequence_ ? public_api_multipart_bit
                                       : public_api_complete_unit;
        const bool take_sync_in_admission =
          needs_sync_ && (old & public_api_sync_bit) == 0;
        if (take_sync_in_admission)
            desired |= public_api_sync_bit;
        if (public_api_state.compare_exchange_weak (
              old, desired, std::memory_order_acq_rel,
              std::memory_order_acquire)) {
            if (needs_sync_) {
                if (take_sync_in_admission)
                    mark_public_api_sync_owned ();
                else
                    lock_public_api_sync ();
            }
            if (sync_locked_out_)
                *sync_locked_out_ = needs_sync_;
            return true;
        }
    }
}

void zlink::socket_lifecycle_coordinator_t::leave_public_send (
  bool sync_locked_, bool multipart_sequence_)
{
    uint64_t delta = 1;
    delta += multipart_sequence_ ? public_api_multipart_bit
                                 : public_api_complete_unit;
    if (sync_locked_)
        delta += public_api_sync_bit;

    if (sync_locked_)
        unmark_public_api_sync_owned ();

    const uint64_t old = public_api_state.fetch_sub (
      delta, std::memory_order_acq_rel);
    zlink_assert ((old & public_api_inflight_mask) > 0);
    if (multipart_sequence_)
        zlink_assert ((old & public_api_multipart_bit) != 0);
    else
        zlink_assert ((old & public_api_complete_mask) != 0);
    if (sync_locked_)
        zlink_assert ((old & public_api_sync_bit) != 0);
}

void zlink::socket_lifecycle_coordinator_t::suspend_public_multipart_send (
  bool sync_locked_)
{
    uint64_t delta = 1;
    if (sync_locked_)
        delta += public_api_sync_bit;

    if (sync_locked_)
        unmark_public_api_sync_owned ();

    const uint64_t old = public_api_state.fetch_sub (
      delta, std::memory_order_acq_rel);
    zlink_assert ((old & public_api_inflight_mask) > 0);
    zlink_assert ((old & public_api_multipart_bit) != 0);
    zlink_assert ((old & public_api_complete_mask) == 0);
    if (sync_locked_)
        zlink_assert ((old & public_api_sync_bit) != 0);
}

bool zlink::socket_lifecycle_coordinator_t::resume_public_multipart_send (
  bool needs_sync_, bool *sync_locked_out_)
{
    if (sync_locked_out_)
        *sync_locked_out_ = false;

    uint64_t old = public_api_state.load (std::memory_order_acquire);
    while (true) {
        if ((old & public_api_closing_bit) != 0) {
            errno = ESHUTDOWN;
            return false;
        }
        if ((old & public_api_multipart_bit) == 0
            || (old & public_api_complete_mask) != 0) {
            errno = EINVAL;
            return false;
        }
        if ((old & public_api_inflight_mask) == public_api_inflight_mask) {
            errno = EBUSY;
            return false;
        }

        uint64_t desired = old + 1;
        const bool take_sync_in_admission =
          needs_sync_ && (old & public_api_sync_bit) == 0;
        if (take_sync_in_admission)
            desired |= public_api_sync_bit;
        if (public_api_state.compare_exchange_weak (
              old, desired, std::memory_order_acq_rel,
              std::memory_order_acquire)) {
            if (needs_sync_) {
                if (take_sync_in_admission)
                    mark_public_api_sync_owned ();
                else
                    lock_public_api_sync ();
            }
            if (sync_locked_out_)
                *sync_locked_out_ = needs_sync_;
            return true;
        }
    }
}

void zlink::socket_lifecycle_coordinator_t::release_public_multipart_marker (
  bool sync_locked_)
{
    uint64_t delta = public_api_multipart_bit;
    if (sync_locked_)
        delta += public_api_sync_bit;

    if (sync_locked_)
        unmark_public_api_sync_owned ();

    const uint64_t old = public_api_state.fetch_sub (
      delta, std::memory_order_acq_rel);
    zlink_assert ((old & public_api_multipart_bit) != 0);
    zlink_assert ((old & public_api_complete_mask) == 0);
    if (sync_locked_)
        zlink_assert ((old & public_api_sync_bit) != 0);
}

bool zlink::socket_lifecycle_coordinator_t::acquire_poller_registration ()
{
    // Admission is needed only while taking the lifetime pin. Keeping a
    // registration in public_api_state would reject the documented close
    // transition and prevent the poller from reporting POLLERR.
    if (!enter_public_api ())
        return false;
    inc_mailbox_ref ();
    leave_public_api ();
    return true;
}

bool zlink::socket_lifecycle_coordinator_t::release_poller_registration ()
{
    return dec_mailbox_ref ();
}

bool zlink::socket_lifecycle_coordinator_t::begin_close_or_fail_busy ()
{
    uint64_t old = public_api_state.load (std::memory_order_acquire);
    while (true) {
        if ((old & public_api_closing_bit) != 0) {
            errno = EALREADY;
            return false;
        }

        const uint64_t inflight = old & public_api_inflight_mask;
        if (inflight != 0) {
            errno = EBUSY;
            return false;
        }
        // An incremental multipart lease can outlive the individual part
        // call, and the async command owner can hold the raw sync bit without
        // a public lifecycle token. Neither is an executing public API.
        // Complete-record admission, however, is always paired with an
        // in-flight token and must be absent here.
        zlink_assert ((old & public_api_complete_mask) == 0);

        const uint64_t desired = old | public_api_closing_bit;
        if (public_api_state.compare_exchange_weak (old, desired, std::memory_order_acq_rel,
                                                    std::memory_order_acquire))
            return true;
    }
}

bool zlink::socket_lifecycle_coordinator_t::public_close_requested () const
{
    return (public_api_state.load (std::memory_order_acquire) & public_api_closing_bit) != 0;
}

bool zlink::socket_lifecycle_coordinator_t::public_multipart_send_active () const
{
    // Read the marker first.  If its release is observed, the acquire also
    // observes the boundary lease published before that release, closing the
    // multipart-to-complete-submit hand-off gap.
    if ((public_api_state.load (std::memory_order_acquire)
         & public_api_multipart_bit)
        != 0)
        return true;
    return public_multipart_control_boundary.load (std::memory_order_acquire);
}

void zlink::socket_lifecycle_coordinator_t::hold_public_multipart_control_boundary ()
{
    public_multipart_control_boundary.store (true, std::memory_order_release);
}

void zlink::socket_lifecycle_coordinator_t::release_public_multipart_control_boundary ()
{
    public_multipart_control_boundary.store (false, std::memory_order_release);
}

bool zlink::socket_lifecycle_coordinator_t::public_api_sync_held () const
{
    return (public_api_state.load (std::memory_order_acquire) & public_api_sync_bit) != 0;
}

bool zlink::socket_lifecycle_coordinator_t::public_api_sync_owned_by_current_thread () const
{
    for (const socket_lifecycle_coordinator_t *owner =
           _current_thread_public_api_sync_owner;
         owner; owner = owner->_previous_thread_public_api_sync_owner) {
        if (owner == this)
            return true;
    }
    return false;
}

void zlink::socket_lifecycle_coordinator_t::mark_public_api_sync_owned ()
{
    zlink_assert (!public_api_sync_owned_by_current_thread ());
    _previous_thread_public_api_sync_owner =
      _current_thread_public_api_sync_owner;
    _current_thread_public_api_sync_owner = this;
}

void zlink::socket_lifecycle_coordinator_t::unmark_public_api_sync_owned ()
{
    zlink_assert (_current_thread_public_api_sync_owner == this);
    _current_thread_public_api_sync_owner =
      _previous_thread_public_api_sync_owner;
    _previous_thread_public_api_sync_owner = NULL;
}

void zlink::socket_lifecycle_coordinator_t::lock_public_api_sync ()
{
    unsigned int attempt = 0;
    uint64_t old = public_api_state.load (std::memory_order_acquire);
    while (true) {
        if ((old & public_api_sync_bit) == 0) {
            const uint64_t desired = old | public_api_sync_bit;
            if (public_api_state.compare_exchange_weak (old, desired, std::memory_order_acquire,
                                                        std::memory_order_acquire)) {
                mark_public_api_sync_owned ();
                return;
            }
            continue;
        }

        public_api_sync_backoff (attempt);
        if (attempt < public_api_sync_yield_limit)
            ++attempt;
        old = public_api_state.load (std::memory_order_acquire);
    }
}

void zlink::socket_lifecycle_coordinator_t::unlock_public_api_sync ()
{
    unmark_public_api_sync_owned ();
    const uint64_t old =
      public_api_state.fetch_and (~public_api_sync_bit, std::memory_order_release);
    zlink_assert ((old & public_api_sync_bit) != 0);
}

void zlink::socket_lifecycle_coordinator_t::unlock_public_api_sync_and_leave ()
{
    unmark_public_api_sync_owned ();
    const uint64_t old =
      public_api_state.fetch_sub (public_api_sync_bit | 1u, std::memory_order_acq_rel);
    zlink_assert ((old & public_api_sync_bit) != 0);
    zlink_assert ((old & public_api_inflight_mask) > 0);
}

zlink::socket_public_send_scope_t::socket_public_send_scope_t (
  socket_lifecycle_coordinator_t &coordinator_,
  bool needs_sync_,
  socket_send_admission_mode_t admission_mode_) :
    _coordinator (&coordinator_),
    _entered (false),
    _needs_sync (needs_sync_),
    _sync_locked (false),
    _admission_mode (admission_mode_),
    _multipart_active (false),
    _multipart_marker_owned (false)
{
    if (_admission_mode != socket_send_admission_none) {
        _entered = _coordinator->enter_public_send (
          _needs_sync, _admission_mode == socket_send_admission_multipart,
          &_sync_locked, &_multipart_active);
        _multipart_marker_owned =
          _entered && _admission_mode == socket_send_admission_multipart;
        return;
    }

    if (_needs_sync) {
        _entered = _coordinator->enter_public_api_and_lock_sync ();
        _sync_locked = _entered;
        return;
    }

    _entered = _coordinator->enter_public_api ();
}

zlink::socket_public_send_scope_t::socket_public_send_scope_t (
  socket_public_send_scope_t &&other_) noexcept :
    _coordinator (other_._coordinator),
    _entered (other_._entered),
    _needs_sync (other_._needs_sync),
    _sync_locked (other_._sync_locked),
    _admission_mode (other_._admission_mode),
    _multipart_active (other_._multipart_active),
    _multipart_marker_owned (other_._multipart_marker_owned)
{
    other_._coordinator = NULL;
    other_._entered = false;
    other_._needs_sync = false;
    other_._sync_locked = false;
    other_._admission_mode = socket_send_admission_none;
    other_._multipart_active = false;
    other_._multipart_marker_owned = false;
}

zlink::socket_public_send_scope_t::~socket_public_send_scope_t ()
{
    if (_multipart_marker_owned) {
        if (_entered) {
            _coordinator->leave_public_send (_sync_locked, true);
        } else {
            _coordinator->release_public_multipart_marker (_sync_locked);
        }
        _entered = false;
        _sync_locked = false;
        _multipart_marker_owned = false;
        return;
    }

    if (!_entered)
        return;

    if (_admission_mode == socket_send_admission_complete) {
        _coordinator->leave_public_send (
          _sync_locked, false);
        return;
    }

    if (_sync_locked)
        _coordinator->unlock_public_api_sync_and_leave ();
    else
        _coordinator->leave_public_api ();
}

bool zlink::socket_public_send_scope_t::should_hold_sync_during_retry (
  bool retry_progress_owner_active_) const
{
    return _sync_locked && !retry_progress_owner_active_;
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

void zlink::socket_public_send_scope_t::suspend_multipart_call ()
{
    if (_admission_mode != socket_send_admission_multipart
        || !_multipart_marker_owned || !_entered)
        return;

    _coordinator->suspend_public_multipart_send (_sync_locked);
    _entered = false;
    _sync_locked = false;
}

bool zlink::socket_public_send_scope_t::resume_multipart_call ()
{
    if (_entered)
        return true;
    if (_admission_mode != socket_send_admission_multipart
        || !_multipart_marker_owned) {
        errno = EFAULT;
        return false;
    }

    _entered = _coordinator->resume_public_multipart_send (
      _needs_sync, &_sync_locked);
    return _entered;
}

bool zlink::socket_public_send_scope_t::lock_multipart_for_close_cleanup ()
{
    if (_entered || _admission_mode != socket_send_admission_multipart
        || !_multipart_marker_owned
        || !_coordinator->public_close_requested ()) {
        errno = EINVAL;
        return false;
    }

    if (_needs_sync && !_sync_locked) {
        _coordinator->lock_public_api_sync ();
        _sync_locked = true;
    }
    return true;
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
    async_processing_done.store (false, std::memory_order_release);
    async_quiesce_pending.store (false, std::memory_order_release);
    async_quiesce_completed.store (false, std::memory_order_release);
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
    //  A monitor may detach after close() already quiesced its mailbox owner.
    //  Do not turn that completed handoff back into a pending one: no async
    //  callback remains to publish the acknowledgement a second time.  A
    //  never-started coordinator is different: stop establishes the pending
    //  handoff that complete_deferred_close_handoff() must wait for.
    if (!async_mailbox_active.load (std::memory_order_acquire)
        && async_quiesce_completed.load (std::memory_order_acquire))
        return;

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
        async_quiesce_completed.store (true, std::memory_order_release);
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

void zlink::socket_lifecycle_coordinator_t::complete_deferred_close_handoff (
  mailbox_t *mailbox_, socket_base_t *socket_, int timeout_ms_)
{
    if (is_async_mailbox_active ()) {
        stop_async_mailbox_processing (mailbox_);
        // schedule_if_needed() alone cannot close the running-handler race:
        // it may observe _scheduled=true just before that handler performs its
        // final active check and clears _scheduled on an empty command pipe.
        // Queue a real no-op command so either that handler reschedules itself
        // from visible data or a new handler is posted after it exits.
        if (mailbox_ && socket_) {
            command_t wake;
            memset (&wake, 0, sizeof (wake));
            wake.destination = socket_;
            wake.type = command_t::request_completion;
            mailbox_->send (wake);
        }
        wait_async_quiesced (timeout_ms_);
    } else if (is_async_quiesce_pending ()) {
        wait_async_quiesced (timeout_ms_);
    }

    if (mailbox_)
        mailbox_->clear_signalers ();
}

void zlink::socket_lifecycle_coordinator_t::mark_destroy_pending ()
{
    destroy_pending.store (true, std::memory_order_release);
}

void zlink::socket_lifecycle_coordinator_t::clear_destroy_pending ()
{
    destroy_pending.store (false, std::memory_order_release);
}

bool zlink::socket_lifecycle_coordinator_t::is_destroy_pending () const
{
    return destroy_pending.load (std::memory_order_acquire);
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
    return static_cast<int> (
      mailbox_ref_state.load (std::memory_order_acquire)
      & mailbox_ref_count_mask);
}

bool zlink::socket_lifecycle_coordinator_t::try_inc_mailbox_ref ()
{
    uint32_t old = mailbox_ref_state.load (std::memory_order_acquire);
    while ((old & mailbox_ref_sealed_bit) == 0) {
        zlink_assert ((old & mailbox_ref_count_mask)
                      != mailbox_ref_count_mask);
        if (mailbox_ref_state.compare_exchange_weak (
              old, old + 1, std::memory_order_acq_rel,
              std::memory_order_acquire))
            return true;
    }
    return false;
}

void zlink::socket_lifecycle_coordinator_t::inc_mailbox_ref ()
{
    const bool acquired = try_inc_mailbox_ref ();
    zlink_assert (acquired);
}

bool zlink::socket_lifecycle_coordinator_t::dec_mailbox_ref ()
{
    const uint32_t old =
      mailbox_ref_state.fetch_sub (1, std::memory_order_acq_rel);
    zlink_assert ((old & mailbox_ref_sealed_bit) == 0);
    zlink_assert ((old & mailbox_ref_count_mask) != 0);
    return ((old - 1) & mailbox_ref_count_mask) != 0;
}

bool zlink::socket_lifecycle_coordinator_t::seal_mailbox_refs_if_zero ()
{
    uint32_t expected = 0;
    return mailbox_ref_state.compare_exchange_strong (
      expected, mailbox_ref_sealed_bit, std::memory_order_acq_rel,
      std::memory_order_acquire);
}

bool zlink::socket_lifecycle_coordinator_t::mailbox_refs_sealed () const
{
    return (mailbox_ref_state.load (std::memory_order_acquire)
            & mailbox_ref_sealed_bit)
           != 0;
}

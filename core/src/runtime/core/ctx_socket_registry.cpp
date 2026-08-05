/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "core/ctx_socket_registry.hpp"

#include "sockets/common/socket_base.hpp"
#include "utils/clock.hpp"

bool zlink::ctx_socket_registry_t::initialize_slot_pool (size_t slot_count_,
                                                         size_t first_socket_slot_,
                                                         i_mailbox *term_mailbox_)
{
    clear ();

    try {
        _slots.resize (slot_count_, NULL);
        _empty_slots.reserve (slot_count_ > first_socket_slot_ ? slot_count_ - first_socket_slot_
                                                               : 0);
    }
    catch (const std::bad_alloc &) {
        errno = ENOMEM;
        clear ();
        return false;
    }

    if (!_slots.empty ())
        _slots[0] = term_mailbox_;

    for (size_t i = slot_count_; i > first_socket_slot_; --i)
        _empty_slots.push_back (static_cast<uint32_t> (i - 1));

    return true;
}

void zlink::ctx_socket_registry_t::clear ()
{
    _sockets.clear ();
    _empty_slots.clear ();
    _slots.clear ();
}

void zlink::ctx_socket_registry_t::bind_mailbox (uint32_t tid_, i_mailbox *mailbox_)
{
    _slots[tid_] = mailbox_;
}

zlink::i_mailbox *zlink::ctx_socket_registry_t::mailbox (uint32_t tid_) const
{
    return tid_ < _slots.size () ? _slots[tid_] : NULL;
}

bool zlink::ctx_socket_registry_t::has_available_socket_slot () const
{
    return !_empty_slots.empty ();
}

uint32_t zlink::ctx_socket_registry_t::claim_socket_slot ()
{
    const uint32_t slot = _empty_slots.back ();
    _empty_slots.pop_back ();
    return slot;
}

void zlink::ctx_socket_registry_t::release_unused_socket_slot (uint32_t slot_)
{
    _empty_slots.push_back (slot_);
    _slots[slot_] = NULL;
}

void zlink::ctx_socket_registry_t::publish_socket (socket_base_t *socket_)
{
    _sockets.push_back (socket_);
    _slots[socket_->get_tid ()] = socket_->get_mailbox ();
}

void zlink::ctx_socket_registry_t::remove_socket (socket_base_t *socket_)
{
    const uint32_t tid = socket_->get_tid ();
    _empty_slots.push_back (tid);
    _slots[tid] = NULL;
    _sockets.erase (socket_);
    _socket_state_cv.broadcast ();
}

bool zlink::ctx_socket_registry_t::empty () const
{
    sockets_t &sockets = const_cast<sockets_t &> (_sockets);
    return sockets.empty ();
}

size_t zlink::ctx_socket_registry_t::socket_count () const
{
    sockets_t &sockets = const_cast<sockets_t &> (_sockets);
    return static_cast<size_t> (sockets.size ());
}

void zlink::ctx_socket_registry_t::collect_sockets (std::vector<socket_base_t *> *out_) const
{
    if (!out_)
        return;

    out_->clear ();
    sockets_t &sockets = const_cast<sockets_t &> (_sockets);
    const sockets_t::size_type size = sockets.size ();
    out_->reserve (size);
    for (sockets_t::size_type i = 0; i != size; ++i)
        out_->push_back (sockets[i]);
}

int zlink::ctx_socket_registry_t::wait_for_socket_removal (mutex_t *sync_,
                                                           const socket_base_t *socket_,
                                                           int timeout_ms_)
{
    if (!socket_)
        return 0;

    const uint64_t deadline_ms = timeout_ms_ >= 0 ? zlink::clock_t ().now_ms () + timeout_ms_ : 0;
    while (true) {
        if (!contains_socket (socket_))
            return 0;
        if (timeout_ms_ == 0) {
            errno = ETIMEDOUT;
            return -1;
        }

        const int wait_ms =
          timeout_ms_ < 0 ? -1 : static_cast<int> (deadline_ms - zlink::clock_t ().now_ms ());
        if (wait_ms <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }

        const int rc = _socket_state_cv.wait (sync_, wait_ms);
        if (rc != 0 && errno == EAGAIN) {
            errno = ETIMEDOUT;
            return -1;
        }
    }
}

int zlink::ctx_socket_registry_t::wait_for_socket_count_at_most (mutex_t *sync_,
                                                                 size_t max_count_,
                                                                 int timeout_ms_)
{
    const uint64_t deadline_ms = timeout_ms_ >= 0 ? zlink::clock_t ().now_ms () + timeout_ms_ : 0;
    while (true) {
        if (socket_count () <= max_count_)
            return 0;
        if (timeout_ms_ == 0) {
            errno = ETIMEDOUT;
            return -1;
        }

        const int wait_ms =
          timeout_ms_ < 0 ? -1 : static_cast<int> (deadline_ms - zlink::clock_t ().now_ms ());
        if (wait_ms <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }

        const int rc = _socket_state_cv.wait (sync_, wait_ms);
        if (rc != 0 && errno == EAGAIN) {
            errno = ETIMEDOUT;
            return -1;
        }
    }
}

bool zlink::ctx_socket_registry_t::contains_socket (const socket_base_t *socket_) const
{
    sockets_t &sockets = const_cast<sockets_t &> (_sockets);
    const sockets_t::size_type size = sockets.size ();
    for (sockets_t::size_type i = 0; i != size; ++i) {
        if (sockets[i] == socket_)
            return true;
    }

    return false;
}

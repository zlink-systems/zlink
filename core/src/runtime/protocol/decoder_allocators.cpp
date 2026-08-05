/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "protocol/decoder_allocators.hpp"

#include "core/msg.hpp"

#include <limits>

namespace
{
std::size_t clamp_allocation_size (std::size_t requested_, std::size_t max_)
{
    if (max_ == 0)
        return 1;
    if (requested_ == 0)
        return 1;
    return requested_ > max_ ? max_ : requested_;
}

std::size_t max_counter_count_for_size (std::size_t max_size_)
{
    const std::size_t stride = zlink::msg_t::max_vsm_size;
    return max_size_ / stride + (max_size_ % stride == 0 ? 0 : 1);
}

bool checked_add (std::size_t lhs_, std::size_t rhs_, std::size_t *out_)
{
    if (lhs_ > std::numeric_limits<std::size_t>::max () - rhs_)
        return false;
    *out_ = lhs_ + rhs_;
    return true;
}

bool checked_mul (std::size_t lhs_, std::size_t rhs_, std::size_t *out_)
{
    if (lhs_ != 0 && rhs_ > std::numeric_limits<std::size_t>::max () / lhs_)
        return false;
    *out_ = lhs_ * rhs_;
    return true;
}

bool compute_allocation_size (std::size_t target_size_,
                              std::size_t max_counters_,
                              std::size_t *out_)
{
    std::size_t counter_bytes = 0;
    if (!checked_mul (max_counters_, sizeof (zlink::msg_t::content_t), &counter_bytes))
        return false;

    std::size_t with_refcount = 0;
    if (!checked_add (target_size_, sizeof (zlink::atomic_counter_t), &with_refcount))
        return false;

    return checked_add (with_refcount, counter_bytes, out_);
}
}

zlink::shared_message_memory_allocator::shared_message_memory_allocator (std::size_t bufsize_) :
    _buf (NULL),
    _buf_size (0),
    _allocation_size (clamp_allocation_size (bufsize_, bufsize_)),
    _max_size (bufsize_),
    _allocated_size (0),
    _msg_content (NULL),
    _max_counters (max_counter_count_for_size (_max_size))
{
}

zlink::shared_message_memory_allocator::shared_message_memory_allocator (
  std::size_t bufsize_, std::size_t max_messages_) :
    _buf (NULL),
    _buf_size (0),
    _allocation_size (clamp_allocation_size (bufsize_, bufsize_)),
    _max_size (bufsize_),
    _allocated_size (0),
    _msg_content (NULL),
    _max_counters (max_messages_)
{
}

zlink::shared_message_memory_allocator::shared_message_memory_allocator (std::size_t bufsize_,
                                                                         std::size_t max_messages_,
                                                                         std::size_t max_size_) :
    _buf (NULL),
    _buf_size (0),
    _allocation_size (
      clamp_allocation_size (bufsize_, max_size_ >= bufsize_ ? max_size_ : bufsize_)),
    _max_size (max_size_ >= bufsize_ ? max_size_ : bufsize_),
    _allocated_size (0),
    _msg_content (NULL),
    _max_counters (max_messages_)
{
}

zlink::shared_message_memory_allocator::~shared_message_memory_allocator ()
{
    deallocate ();
}

unsigned char *zlink::shared_message_memory_allocator::allocate ()
{
    const std::size_t target_size = clamp_allocation_size (_allocation_size, _max_size);
    _allocation_size = target_size;

    if (_buf) {
        // release reference count to couple lifetime to messages
        zlink::atomic_counter_t *c = reinterpret_cast<zlink::atomic_counter_t *> (_buf);

        // if refcnt drops to 0, there are no message using the buffer
        // because either all messages have been closed or only vsm-messages
        // were created
        if (c->sub (1)) {
            // buffer is still in use as message data. "Release" it and create a new one
            // release pointer because we are going to create a new buffer
            release ();
        } else if (_allocated_size != target_size) {
            c->~atomic_counter_t ();
            std::free (_buf);
            clear ();
        }
    }

    // if buf != NULL it is not used by any message so we can re-use it for the next run
    if (!_buf) {
        // allocate memory for reference counters together with reception buffer
        std::size_t allocationsize = 0;
        if (!compute_allocation_size (target_size, _max_counters, &allocationsize)) {
            errno = ENOMEM;
            alloc_assert (false);
        }

        _buf = static_cast<unsigned char *> (std::malloc (allocationsize));
        alloc_assert (_buf);

        new (_buf) atomic_counter_t (1);
        _allocated_size = target_size;
    } else {
        // release reference count to couple lifetime to messages
        zlink::atomic_counter_t *c = reinterpret_cast<zlink::atomic_counter_t *> (_buf);
        c->set (1);
    }

    _buf_size = target_size;
    _msg_content = reinterpret_cast<zlink::msg_t::content_t *> (_buf + sizeof (atomic_counter_t)
                                                                + _allocated_size);
    return _buf + sizeof (zlink::atomic_counter_t);
}

void zlink::shared_message_memory_allocator::deallocate ()
{
    zlink::atomic_counter_t *c = reinterpret_cast<zlink::atomic_counter_t *> (_buf);
    if (_buf && !c->sub (1)) {
        c->~atomic_counter_t ();
        std::free (_buf);
    }
    clear ();
}

unsigned char *zlink::shared_message_memory_allocator::release ()
{
    unsigned char *b = _buf;
    clear ();
    return b;
}

void zlink::shared_message_memory_allocator::clear ()
{
    _buf = NULL;
    _buf_size = 0;
    _allocated_size = 0;
    _msg_content = NULL;
}

void zlink::shared_message_memory_allocator::inc_ref ()
{
    (reinterpret_cast<zlink::atomic_counter_t *> (_buf))->add (1);
}

void zlink::shared_message_memory_allocator::call_dec_ref (void *, void *hint_)
{
    zlink_assert (hint_);
    unsigned char *buf = static_cast<unsigned char *> (hint_);
    zlink::atomic_counter_t *c = reinterpret_cast<zlink::atomic_counter_t *> (buf);

    if (!c->sub (1)) {
        c->~atomic_counter_t ();
        std::free (buf);
        buf = NULL;
    }
}


std::size_t zlink::shared_message_memory_allocator::size () const
{
    return _buf_size;
}

unsigned char *zlink::shared_message_memory_allocator::data ()
{
    return _buf + sizeof (zlink::atomic_counter_t);
}

void zlink::shared_message_memory_allocator::resize (std::size_t new_size_)
{
    const std::size_t clamped = clamp_allocation_size (new_size_, _allocation_size);
    if (clamped >= _buf_size) {
        _buf_size = clamped;
        return;
    }

    // Avoid frequent shrink/grow oscillation on bursty read sizes.
    if (_buf_size == 0 || clamped * 2 <= _buf_size)
        _buf_size = clamped;
}

void zlink::shared_message_memory_allocator::set_allocation_size (std::size_t new_size_)
{
    _allocation_size = clamp_allocation_size (new_size_, _max_size);
    if (_buf_size > _allocation_size || _buf_size == 0)
        _buf_size = _allocation_size;
}

#ifdef ZLINK_BUILD_TESTS
bool zlink::shared_message_memory_allocator::allocation_size_for_test (std::size_t target_size_,
                                                                       std::size_t max_counters_,
                                                                       std::size_t *out_)
{
    return compute_allocation_size (target_size_, max_counters_, out_);
}
#endif

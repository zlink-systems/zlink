/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "protocol/decoder_allocators.hpp"

#include "core/msg.hpp"
#include "utils/atomic_ptr.hpp"

#include <limits>
#include <new>

struct zlink::shared_message_memory_allocator_state_t
{
    explicit shared_message_memory_allocator_state_t (
      std::size_t max_spare_size_) :
        references (1), max_spare_size (max_spare_size_), closed_marker (0)
    {
    }

    atomic_counter_t references;
    atomic_ptr_t<unsigned char> spare;
    const std::size_t max_spare_size;
    unsigned char closed_marker;
};

namespace
{
struct shared_message_memory_buffer_header_t
{
    shared_message_memory_buffer_header_t (
      zlink::shared_message_memory_allocator_state_t *owner_,
      std::size_t allocated_size_) :
        references (1), owner (owner_), allocated_size (allocated_size_)
    {
    }

    zlink::atomic_counter_t references;
    zlink::shared_message_memory_allocator_state_t *owner;
    std::size_t allocated_size;
};

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
    if (!checked_add (target_size_, sizeof (shared_message_memory_buffer_header_t),
                      &with_refcount))
        return false;

    return checked_add (with_refcount, counter_bytes, out_);
}

zlink::shared_message_memory_allocator_state_t *create_recycle_state (
  std::size_t max_spare_size_)
{
    zlink::shared_message_memory_allocator_state_t *state =
      new (std::nothrow)
        zlink::shared_message_memory_allocator_state_t (max_spare_size_);
    alloc_assert (state);
    return state;
}

void release_recycle_state (
  zlink::shared_message_memory_allocator_state_t *state_)
{
    if (!state_->references.sub (1))
        delete state_;
}

shared_message_memory_buffer_header_t *buffer_header (unsigned char *buffer_)
{
    return reinterpret_cast<shared_message_memory_buffer_header_t *> (buffer_);
}

unsigned char *closed_sentinel (
  zlink::shared_message_memory_allocator_state_t *state_)
{
    return &state_->closed_marker;
}

void destroy_buffer (unsigned char *buffer_)
{
    shared_message_memory_buffer_header_t *header = buffer_header (buffer_);
    zlink::shared_message_memory_allocator_state_t *const state = header->owner;
    header->~shared_message_memory_buffer_header_t ();
    std::free (buffer_);
    release_recycle_state (state);
}

void recycle_or_destroy_buffer (unsigned char *buffer_)
{
    shared_message_memory_buffer_header_t *const header = buffer_header (buffer_);
    zlink::shared_message_memory_allocator_state_t *const state = header->owner;

    // Publish at most one normal read-size block. The compare/exchange release
    // makes the initialized header visible to the decoder-side consumer. An
    // occupied slot or the terminal CLOSED sentinel rejects the returned block.
    if (header->allocated_size <= state->max_spare_size
        && state->spare.cas (NULL, buffer_) == NULL)
        return;

    // Additional or dynamically grown returns are freed immediately so
    // backlog draining cannot grow either the pool or its retained bytes.
    destroy_buffer (buffer_);
}

unsigned char *take_exact_spare (
  zlink::shared_message_memory_allocator_state_t *state_,
  std::size_t target_size_)
{
    // Only the live decoder consumes the slot. Allocator methods do not race
    // its destructor, so CLOSED cannot be observed on this path.
    unsigned char *const spare = state_->spare.xchg (NULL);
    zlink_assert (spare != closed_sentinel (state_));
    if (!spare)
        return NULL;

    // A different capacity is never reused: the payload/content layout is
    // derived from the exact allocation target.
    if (buffer_header (spare)->allocated_size != target_size_) {
        destroy_buffer (spare);
        return NULL;
    }

    buffer_header (spare)->references.set (1);
    return spare;
}

unsigned char *allocate_buffer (
  zlink::shared_message_memory_allocator_state_t *state_,
  std::size_t target_size_, std::size_t max_counters_)
{
    std::size_t allocation_size = 0;
    if (!compute_allocation_size (target_size_, max_counters_, &allocation_size)) {
        errno = ENOMEM;
        alloc_assert (false);
    }

    unsigned char *const buffer =
      static_cast<unsigned char *> (std::malloc (allocation_size));
    alloc_assert (buffer);

    state_->references.add (1);
    new (buffer) shared_message_memory_buffer_header_t (state_, target_size_);
    return buffer;
}

unsigned char *stop_recycling (
  zlink::shared_message_memory_allocator_state_t *state_)
{
    unsigned char *const closed = closed_sentinel (state_);
    unsigned char *const spare = state_->spare.xchg (closed);

    // If publication wins first, shutdown acquires and owns the block. If
    // shutdown wins first, the publisher observes CLOSED and frees its block.
    return spare == closed ? NULL : spare;
}
}

zlink::shared_message_memory_allocator::shared_message_memory_allocator (std::size_t bufsize_) :
    _buf (NULL),
    _buf_size (0),
    _allocation_size (clamp_allocation_size (bufsize_, bufsize_)),
    _max_size (bufsize_),
    _allocated_size (0),
    _msg_content (NULL),
    _max_counters (max_counter_count_for_size (_max_size)),
    _recycle_state (create_recycle_state (_allocation_size))
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
    _max_counters (max_messages_),
    _recycle_state (create_recycle_state (_allocation_size))
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
    _max_counters (max_messages_),
    _recycle_state (create_recycle_state (_allocation_size))
{
}

zlink::shared_message_memory_allocator::~shared_message_memory_allocator ()
{
    // Stop publication before dropping the decoder-owned reference. A buffer
    // still held by a message keeps the recycle state alive until that message
    // closes, but can no longer publish storage to a destroyed allocator.
    unsigned char *const spare = stop_recycling (_recycle_state);
    deallocate ();
    if (spare)
        destroy_buffer (spare);
    release_recycle_state (_recycle_state);
    _recycle_state = NULL;
}

unsigned char *zlink::shared_message_memory_allocator::allocate ()
{
    const std::size_t target_size = clamp_allocation_size (_allocation_size, _max_size);
    _allocation_size = target_size;

    if (_buf) {
        shared_message_memory_buffer_header_t *const header =
          buffer_header (_buf);

        // if refcnt drops to 0, there are no message using the buffer
        // because either all messages have been closed or only vsm-messages
        // were created
        if (header->references.sub (1)) {
            // buffer is still in use as message data. "Release" it and create a new one
            // release pointer because we are going to create a new buffer
            release ();
        } else if (header->allocated_size != target_size) {
            destroy_buffer (_buf);
            clear ();
        }
    }

    // if buf != NULL it is not used by any message so we can re-use it for the next run
    if (!_buf) {
        _buf = take_exact_spare (_recycle_state, target_size);
        if (!_buf)
            _buf = allocate_buffer (_recycle_state, target_size,
                                    _max_counters);
    } else {
        buffer_header (_buf)->references.set (1);
    }

    _allocated_size = buffer_header (_buf)->allocated_size;
    _buf_size = target_size;
    _msg_content = reinterpret_cast<zlink::msg_t::content_t *> (
      _buf + sizeof (shared_message_memory_buffer_header_t)
      + _allocated_size);
    return _buf + sizeof (shared_message_memory_buffer_header_t);
}

void zlink::shared_message_memory_allocator::deallocate ()
{
    if (_buf && !buffer_header (_buf)->references.sub (1))
        recycle_or_destroy_buffer (_buf);
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
    buffer_header (_buf)->references.add (1);
}

void zlink::shared_message_memory_allocator::call_dec_ref (void *, void *hint_)
{
    zlink_assert (hint_);
    unsigned char *buf = static_cast<unsigned char *> (hint_);
    if (!buffer_header (buf)->references.sub (1))
        recycle_or_destroy_buffer (buf);
}


std::size_t zlink::shared_message_memory_allocator::size () const
{
    return _buf_size;
}

unsigned char *zlink::shared_message_memory_allocator::data ()
{
    return _buf + sizeof (shared_message_memory_buffer_header_t);
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

std::size_t zlink::shared_message_memory_allocator::buffer_header_size_for_test ()
{
    return sizeof (shared_message_memory_buffer_header_t);
}

std::size_t zlink::shared_message_memory_allocator::cached_buffer_count_for_test ()
{
    unsigned char *const spare = _recycle_state->spare.cas (NULL, NULL);
    return spare && spare != closed_sentinel (_recycle_state) ? 1 : 0;
}
#endif

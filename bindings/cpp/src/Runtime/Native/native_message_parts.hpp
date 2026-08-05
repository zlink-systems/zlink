/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_NATIVE_MESSAGE_PARTS_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_NATIVE_MESSAGE_PARTS_HPP_INCLUDED

#include <Runtime/Native/message_access.hpp>

#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/Contracts/Sockets/results.hpp>

#include <array>
#include <cerrno>
#include <utility>
#include <vector>

namespace zlink
{
namespace detail
{

constexpr size_t native_part_stack_capacity = 8u;

inline void close_message_array (zlink_msg_t *parts_, size_t part_count_) noexcept
{
    if (!parts_)
        return;
    zlink_multipart_close (parts_, part_count_);
}

inline void close_native_parts (std::vector<zlink_msg_t> &parts_, size_t start_index_ = 0) noexcept
{
    if (start_index_ >= parts_.size ())
        return;

    for (size_t i = start_index_; i < parts_.size (); ++i)
        (void) zlink_msg_close (&parts_[i]);
}

inline void
close_native_parts (zlink_msg_t *parts_, size_t part_count_, size_t start_index_ = 0) noexcept
{
    if (!parts_ || start_index_ >= part_count_)
        return;

    for (size_t i = start_index_; i < part_count_; ++i)
        (void) zlink_msg_close (&parts_[i]);
}

inline int move_parts_to_native (std::vector<message_t> &parts_, std::vector<zlink_msg_t> &native_)
{
    native_.clear ();
    native_.resize (parts_.size ());

    size_t moved = 0;
    for (; moved < parts_.size (); ++moved) {
        if (!parts_[moved].valid ()) {
            errno = EINVAL;
            break;
        }
        detail::move_to_native (parts_[moved], &native_[moved]);
        if (parts_[moved].valid ())
            break;
    }

    if (moved == parts_.size ())
        return 0;

    for (size_t i = 0; i < moved; ++i) {
        parts_[i].init ();
        if (parts_[i].valid ())
            (void) zlink_msg_move (detail::native_handle (parts_[i]), &native_[i]);
        (void) zlink_msg_close (&native_[i]);
    }

    native_.clear ();
    return -1;
}

inline void restore_part_from_native (message_t &part_, zlink_msg_t &native_) noexcept
{
    part_.init ();
    if (part_.valid ())
        (void) zlink_msg_move (detail::native_handle (part_), &native_);
    (void) zlink_msg_close (&native_);
}

inline void restore_parts_from_native (std::vector<message_t> &parts_,
                                       std::vector<zlink_msg_t> &native_,
                                       size_t start_index_ = 0) noexcept
{
    const size_t count = native_.size () < parts_.size () ? native_.size () : parts_.size ();
    for (size_t i = start_index_; i < count; ++i) {
        parts_[i].init ();
        if (parts_[i].valid ())
            (void) zlink_msg_move (detail::native_handle (parts_[i]), &native_[i]);
        (void) zlink_msg_close (&native_[i]);
    }
    native_.clear ();
}

inline int
move_parts_to_native (std::vector<message_t> &parts_, zlink_msg_t *native_, size_t native_count_)
{
    if (native_count_ != parts_.size ()) {
        errno = EINVAL;
        return -1;
    }

    size_t moved = 0;
    for (; moved < parts_.size (); ++moved) {
        if (!parts_[moved].valid ()) {
            errno = EINVAL;
            break;
        }
        detail::move_to_native (parts_[moved], &native_[moved]);
        if (parts_[moved].valid ())
            break;
    }

    if (moved == parts_.size ())
        return 0;

    for (size_t i = 0; i < moved; ++i)
        restore_part_from_native (parts_[i], native_[i]);
    return -1;
}

inline void restore_parts_from_native (std::vector<message_t> &parts_,
                                       zlink_msg_t *native_,
                                       size_t native_count_,
                                       size_t start_index_ = 0) noexcept
{
    const size_t count = native_count_ < parts_.size () ? native_count_ : parts_.size ();
    for (size_t i = start_index_; i < count; ++i)
        restore_part_from_native (parts_[i], native_[i]);
}

inline int assign_parts_from_native (zlink_msg_t *parts_native_,
                                     size_t part_count_,
                                     std::vector<message_t> &parts_)
{
    parts_.clear ();
    parts_.resize (part_count_);
    for (size_t i = 0; i < part_count_; ++i) {
        if (zlink_msg_move (detail::native_handle (parts_[i]), &parts_native_[i]) != 0) {
            parts_.clear ();
            close_message_array (parts_native_, part_count_);
            return -1;
        }
    }
    close_message_array (parts_native_, part_count_);
    return 0;
}

inline int assign_parts_from_native (std::vector<zlink_msg_t> &parts_native_,
                                     std::vector<message_t> &parts_)
{
    parts_.clear ();
    parts_.resize (parts_native_.size ());
    for (size_t i = 0; i < parts_native_.size (); ++i) {
        if (zlink_msg_move (detail::native_handle (parts_[i]), &parts_native_[i]) != 0) {
            parts_.clear ();
            close_native_parts (parts_native_, i);
            parts_native_.clear ();
            return -1;
        }
    }
    parts_native_.clear ();
    return 0;
}

inline std::vector<message_t> take_parts_from_native (zlink_msg_t *parts_, size_t part_count_)
{
    std::vector<message_t> parts;
    parts.resize (part_count_);
    for (size_t i = 0; i < part_count_; ++i)
        (void) zlink_msg_move (detail::native_handle (parts[i]), &parts_[i]);
    close_message_array (parts_, part_count_);
    return parts;
}

template <typename SubmitFn>
inline int submit_native_parts (std::vector<zlink_msg_t> &parts_native_,
                                size_t &failed_index_out_,
                                SubmitFn submit_)
{
    failed_index_out_ = 0;
    if (parts_native_.empty ()) {
        errno = EFAULT;
        return ZLINK_SUBMIT_INVALID_HANDLE;
    }

    for (size_t i = 0; i < parts_native_.size (); ++i) {
        const zlink_part_flag_t part_flag =
          i + 1 < parts_native_.size () ? ZLINK_PART_MORE : ZLINK_PART_FINAL;
        const int rc = submit_ (&parts_native_[i], part_flag, i + 1 == parts_native_.size ());
        if (rc != ZLINK_SUBMIT_OK) {
            failed_index_out_ = i;
            return rc;
        }
    }

    return ZLINK_SUBMIT_OK;
}

template <typename SubmitFn>
inline int submit_native_parts (zlink_msg_t *parts_native_,
                                size_t part_count_,
                                size_t &failed_index_out_,
                                SubmitFn submit_)
{
    failed_index_out_ = 0;
    if (!parts_native_ || part_count_ == 0) {
        errno = EFAULT;
        return ZLINK_SUBMIT_INVALID_HANDLE;
    }

    for (size_t i = 0; i < part_count_; ++i) {
        const zlink_part_flag_t part_flag =
          i + 1 < part_count_ ? ZLINK_PART_MORE : ZLINK_PART_FINAL;
        const int rc = submit_ (&parts_native_[i], part_flag, i + 1 == part_count_);
        if (rc != ZLINK_SUBMIT_OK) {
            failed_index_out_ = i;
            return rc;
        }
    }

    return ZLINK_SUBMIT_OK;
}

template <typename SubmitFn> inline int submit_one_message_part (message_t &part_, SubmitFn submit_)
{
    if (!part_.valid ()) {
        errno = EINVAL;
        return -1;
    }

    zlink_msg_t native_part;
    detail::move_to_native (part_, &native_part);
    if (part_.valid ())
        return -1;

    const int rc = submit_ (&native_part, ZLINK_PART_FINAL);
    if (rc != 0)
        restore_part_from_native (part_, native_part);
    return rc;
}

template <typename BodyFn>
inline int with_moved_native_parts (std::vector<message_t> &parts_, BodyFn body_)
{
    if (parts_.size () <= native_part_stack_capacity) {
        std::array<zlink_msg_t, native_part_stack_capacity> native_parts;
        if (detail::move_parts_to_native (parts_, native_parts.data (), parts_.size ()) != 0)
            return -1;

        return body_ (native_parts.data (), parts_.size ());
    }

    std::vector<zlink_msg_t> native_parts;
    if (detail::move_parts_to_native (parts_, native_parts) != 0)
        return -1;

    return body_ (native_parts.data (), native_parts.size ());
}

template <typename SubmitFn>
inline int submit_message_parts (std::vector<message_t> &parts_, SubmitFn submit_)
{
    return detail::with_moved_native_parts (
      parts_, [&] (zlink_msg_t *native_parts_, size_t part_count_) {
          size_t failed_index = 0;
          const int rc = detail::submit_native_parts (native_parts_, part_count_, failed_index,
                                                      std::move (submit_));
          if (rc != 0)
              detail::restore_parts_from_native (parts_, native_parts_, part_count_, failed_index);
          return rc;
      });
}

template <typename SubmitFn>
inline int submit_message_parts_close_on_failure (std::vector<message_t> &parts_, SubmitFn submit_)
{
    return detail::with_moved_native_parts (
      parts_, [&] (zlink_msg_t *native_parts_, size_t part_count_) {
          size_t failed_index = 0;
          const int rc = detail::submit_native_parts (native_parts_, part_count_, failed_index,
                                                      std::move (submit_));
          if (rc != 0)
              detail::close_native_parts (native_parts_, part_count_, failed_index);
          return rc;
      });
}

//  Builds a borrowed, zero-copy native view over @p parts_ and runs @p body_.
//
//  Send/request/reply operations carry parts as borrowed/read-only: Core
//  keeps caller ownership on both success and failure and copies synchronously
//  what it needs. Unlike the raw-socket move/consume path this adapter never
//  moves, mutates, or invalidates the caller's messages. Each temporary native
//  part shares the source message's reference-counted storage, so a Core copy
//  remains valid after this adapter closes its temporary parts. The caller
//  messages remain valid in every outcome.
template <typename BodyFn>
inline int with_borrowed_native_parts (const std::vector<message_t> &parts_, BodyFn body_)
{
    const size_t n = parts_.size ();
    if (n == 0)
        return body_ (nullptr, 0);
    for (size_t i = 0; i < n; ++i) {
        if (!parts_[i].valid ()) {
            errno = EINVAL;
            return -1;
        }
    }

    auto run = [&] (zlink_msg_t *views_, size_t count_) -> int {
        size_t built = 0;
        for (; built < count_; ++built) {
            const zlink_msg_t *src = detail::native_handle (parts_[built]);
            int irc = zlink_msg_init (&views_[built]);
            if (irc == 0
                && zlink_msg_copy (&views_[built], const_cast<zlink_msg_t *> (src)) != 0) {
                (void) zlink_msg_close (&views_[built]);
                irc = -1;
            }
            if (irc != 0) {
                errno = EINVAL;
                break;
            }
        }
        if (built != count_) {
            for (size_t i = 0; i < built; ++i)
                (void) zlink_msg_close (&views_[i]);
            return -1;
        }

        const int rc = body_ (views_, count_);
        for (size_t i = 0; i < count_; ++i)
            (void) zlink_msg_close (&views_[i]);
        return rc;
    };

    if (n <= native_part_stack_capacity) {
        std::array<zlink_msg_t, native_part_stack_capacity> views;
        return run (views.data (), n);
    }
    std::vector<zlink_msg_t> views (n);
    return run (views.data (), n);
}

template <typename SubmitFn>
inline int submit_borrowed_message_array (const std::vector<message_t> &parts_, SubmitFn submit_)
{
    return detail::with_borrowed_native_parts (
      parts_, [&] (zlink_msg_t *native_parts_, size_t part_count_) {
          return submit_ (native_parts_, part_count_);
      });
}

} // namespace detail
} // namespace zlink

#endif

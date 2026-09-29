/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_NATIVE_MESSAGE_PARTS_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_NATIVE_MESSAGE_PARTS_HPP_INCLUDED

#include <Runtime/Native/message_access.hpp>

#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Errors/errors.hpp>
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

//  Moves a native frame into an already-valid @p part_ and re-establishes the
//  binding-side payload-presence metadata.
//
//  `move_to_native()` and `message_t::init()` both clear `_has_payload`, so
//  every helper that pushes a native payload back into a `message_t` must
//  restore that flag; otherwise a message holding a real payload advertises
//  itself as empty and the receive fast path (which branches on
//  `has_payload()`) can destroy that payload on a failed receive. These are
//  recovery/materialization paths, not the hot single-part path, so the extra
//  `zlink_msg_size()` query is acceptable here.
inline config_result_t adopt_native_part (message_t &part_, zlink_msg_t &native_)
{
    detail::message_access_t::close_noexcept (part_);
    const config_result_t result = static_cast<config_result_t> (
      zlink_msg_adopt (detail::native_handle (part_), &native_));
    if (result != config_result_t::ok)
        return result;
    detail::message_access_t::valid (part_) = true;
    detail::refresh_payload_presence (part_);
    return result;
}

inline void restore_part_from_native (message_t &part_, zlink_msg_t &native_)
{
    const config_result_t result = adopt_native_part (part_, native_);
    const int saved_errno = result != config_result_t::ok ? zlink_errno () : 0;
    (void) zlink_msg_close (&native_);
    detail::throw_if_failed<config_error_t> (result, saved_errno);
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

    for (size_t i = 0; i < moved; ++i)
        restore_part_from_native (parts_[i], native_[i]);

    native_.clear ();
    return -1;
}

inline void restore_parts_from_native (std::vector<message_t> &parts_,
                                       std::vector<zlink_msg_t> &native_,
                                       size_t start_index_ = 0)
{
    const size_t count = native_.size () < parts_.size () ? native_.size () : parts_.size ();
    for (size_t i = start_index_; i < count; ++i)
        restore_part_from_native (parts_[i], native_[i]);
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
                                       size_t start_index_ = 0)
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
        const config_result_t result = adopt_native_part (parts_[i], parts_native_[i]);
        if (result != config_result_t::ok) {
            const int saved_errno = zlink_errno ();
            parts_.clear ();
            close_message_array (parts_native_, part_count_);
            throw config_error_t (result, saved_errno);
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
        const config_result_t result = adopt_native_part (parts_[i], parts_native_[i]);
        if (result != config_result_t::ok) {
            const int saved_errno = zlink_errno ();
            parts_.clear ();
            close_native_parts (parts_native_, i);
            parts_native_.clear ();
            throw config_error_t (result, saved_errno);
        }
    }
    parts_native_.clear ();
    return 0;
}

inline std::vector<message_t> take_parts_from_native (zlink_msg_t *parts_, size_t part_count_)
{
    std::vector<message_t> parts;
    parts.resize (part_count_);
    for (size_t i = 0; i < part_count_; ++i) {
        const config_result_t result = adopt_native_part (parts[i], parts_[i]);
        if (result != config_result_t::ok) {
            const int saved_errno = zlink_errno ();
            close_message_array (parts_, part_count_);
            throw config_error_t (result, saved_errno);
        }
    }
    close_message_array (parts_, part_count_);
    return parts;
}

template <typename SubmitFn> inline int submit_one_message_part (message_t &part_, SubmitFn submit_)
{
    if (!part_.valid ()) {
        errno = EINVAL;
        return ZLINK_SUBMIT_INVALID_ARGUMENT;
    }

    zlink_msg_t native_part;
    detail::move_to_native (part_, &native_part);
    if (part_.valid ())
        return ZLINK_SUBMIT_INVALID_ARGUMENT;

    const int rc = submit_ (&native_part, 1u);
    if (rc != 0)
        restore_part_from_native (part_, native_part);
    return rc;
}

template <typename SubmitFn>
inline int submit_borrowed_message_part (message_t &part_, SubmitFn submit_)
{
    if (!part_.valid ()) {
        errno = EINVAL;
        return ZLINK_SUBMIT_INVALID_ARGUMENT;
    }

    zlink_msg_t native_view;
    const int init_rc = zlink_msg_init (&native_view);
    if (init_rc != 0)
        throw config_error_t (static_cast<config_result_t> (init_rc), zlink_errno ());
    const int copy_rc = zlink_msg_copy (&native_view, detail::native_handle (part_));
    if (copy_rc != 0) {
        const int saved_errno = errno;
        (void) zlink_msg_close (&native_view);
        throw config_error_t (static_cast<config_result_t> (copy_rc), saved_errno);
    }

    const int rc = submit_ (&native_view, 1u);
    const int saved_errno = errno;
    (void) zlink_msg_close (&native_view);
    if (rc == ZLINK_SUBMIT_OK)
        detail::message_access_t::close_noexcept (part_);
    errno = saved_errno;
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
inline int submit_message_parts_close_on_failure (std::vector<message_t> &parts_, SubmitFn submit_)
{
    return detail::with_moved_native_parts (
      parts_, [&] (zlink_msg_t *native_parts_, size_t part_count_) {
          return submit_ (native_parts_, part_count_);
      });
}

//  Builds a borrowed, zero-copy native view over @p parts_ and runs @p body_.
//
//  Send/request/reply operations expose borrowed/read-only C++ messages while
//  submitting independent native views. Core consumes each native argument on
//  success and ordinary failure, but that consumption affects only the view,
//  not the public message. Each view shares the source message's
//  reference-counted storage, so a Core copy remains valid after this adapter
//  closes its temporary parts. The caller messages remain valid in every
//  outcome.
template <typename BodyFn>
inline int with_borrowed_native_parts (const std::vector<message_t> &parts_, BodyFn body_)
{
    const size_t n = parts_.size ();
    if (n == 0)
        return body_ (nullptr, 0);
    for (size_t i = 0; i < n; ++i) {
        if (!parts_[i].valid ()) {
            errno = EINVAL;
            return ZLINK_SUBMIT_INVALID_ARGUMENT;
        }
    }

    auto run = [&] (zlink_msg_t *views_, size_t count_) -> int {
        size_t built = 0;
        for (; built < count_; ++built) {
            const zlink_msg_t *src = detail::native_handle (parts_[built]);
            const int init_rc = zlink_msg_init (&views_[built]);
            int irc = init_rc;
            if (irc == 0) {
                irc = zlink_msg_copy (&views_[built], const_cast<zlink_msg_t *> (src));
            }
            const int saved_errno = irc != 0 ? zlink_errno () : 0;
            if (irc != 0 && init_rc == 0) {
                (void) zlink_msg_close (&views_[built]);
            }
            if (irc != 0) {
                for (size_t i = 0; i < built; ++i)
                    (void) zlink_msg_close (&views_[i]);
                throw config_error_t (static_cast<config_result_t> (irc), saved_errno);
            }
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

// Keep the public C++ messages separate from the per-call native arguments.
// Core consumes an attempted native part even when it rejects the record, so
// submit shallow native views and leave the public messages untouched on
// failure. Close the public messages only after every part succeeds so the C++
// consume-on-success contract is applied to the record as one unit.
template <typename SubmitFn>
inline int submit_message_parts (std::vector<message_t> &parts_, SubmitFn submit_)
{
    const int rc = detail::submit_borrowed_message_array (parts_, std::move (submit_));
    if (rc == ZLINK_SUBMIT_OK) {
        for (message_t &part : parts_)
            detail::message_access_t::close_noexcept (part);
    }
    return rc;
}

} // namespace detail
} // namespace zlink

#endif

/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_MESSAGING_OPERATION_DETAIL_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_MESSAGING_OPERATION_DETAIL_HPP_INCLUDED

#include "../Native/native_message_parts.hpp"
#include "../Native/native_options.hpp"
#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Errors/errors.hpp>
#include <zlink/Contracts/Errors/results.hpp>
#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/Contracts/Sockets/results.hpp>

#include <cerrno>
#include <utility>
#include <vector>

namespace zlink
{

namespace detail
{

using zlink::detail::assign_parts_from_native;
using zlink::detail::close_message_array;
using zlink::detail::close_native_parts;
using zlink::detail::last_error;
using zlink::detail::move_parts_to_native;
using zlink::detail::restore_parts_from_native;
using zlink::detail::submit_borrowed_message_array;
using zlink::detail::submit_message_parts;
using zlink::detail::submit_message_parts_close_on_failure;
using zlink::detail::submit_native_parts;
using zlink::detail::take_parts_from_native;
using zlink::detail::throw_if_failed;

// Moves message ownership into the native slot, rejecting the submission when
// the move does not consume the message (i.e. the message was invalid).
// Centralizes the ownership-transfer protocol shared by every single-part
// submit path; the native slot is closed before the rejection so it never
// leaks on the error edge.
inline void move_to_native_or_reject (message_t &message_, zlink_msg_t *native_)
{
    zlink::detail::move_to_native (message_, native_);
    if (message_.valid ()) {
        (void) zlink_msg_close (native_);
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    }
}

} // namespace detail


} // namespace zlink

#endif

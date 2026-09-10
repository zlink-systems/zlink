/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_API_SOCKET_REQUEST_REPLY_SUBMIT_INTERNAL_HPP_INCLUDED__
#define __ZLINK_API_SOCKET_REQUEST_REPLY_SUBMIT_INTERNAL_HPP_INCLUDED__

#include "api/socket/socket_request_reply_internal.hpp"

namespace zlink
{
namespace socket_reqrep_internal
{
int validate_socket_type (const socket_handle_t &handle_, int expected_type_);
}
}

#endif

/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include <atomic>
#include <zlink/socket/api.h>

#if !defined(_WIN32)
namespace zlink::framework::test::m6a
{
extern std::atomic_bool count_request_submissions;
extern std::atomic_int request_submissions;
extern std::atomic_bool fail_next_disconnect_rid;
extern std::atomic_int injected_disconnect_rid_calls;
extern std::atomic<zlink_connect_result_t> injected_disconnect_result;
}
#endif

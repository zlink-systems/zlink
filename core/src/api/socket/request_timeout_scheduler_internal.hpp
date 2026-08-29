/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_API_REQUEST_TIMEOUT_SCHEDULER_INTERNAL_HPP_INCLUDED__
#define __ZLINK_API_REQUEST_TIMEOUT_SCHEDULER_INTERNAL_HPP_INCLUDED__

#include <memory>
#include <stdint.h>

namespace zlink
{
namespace request_timeout
{
struct task_t;

typedef void (*handler_fn) (void *userdata_);
typedef void (*cleanup_fn) (void *userdata_);

std::shared_ptr<task_t>
schedule (uint32_t timeout_ms_, handler_fn handler_, void *userdata_, cleanup_fn cleanup_ = NULL);
void cancel (const std::shared_ptr<task_t> &task_);
uint64_t monotonic_now_ns ();
uint64_t deadline_after_ms (uint32_t timeout_ms_);

#ifdef ZLINK_BUILD_TESTS
void test_reset_cancel_notification_count ();
uint64_t test_cancel_notification_count ();
#endif
}
}

#endif

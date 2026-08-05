/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/channels/call.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace zlink::framework::runtime::messaging
{

struct submit_attempt_context_t
{
    std::string target;
    const void *owner = nullptr;
    std::uint64_t owner_epoch = 0;
    std::chrono::milliseconds timeout{std::chrono::seconds (1)};
    std::size_t capacity = 1024;
};

void note_submit_attempt (std::string target,
                          const void *owner,
                          std::chrono::milliseconds timeout = std::chrono::seconds (1),
                          std::size_t capacity = 1024);

void notify_submit_ready (const std::string &target, const void *owner);
void notify_submit_ready (const void *owner);
void activate_submit_owner (const void *owner);
void shutdown_submit_owner (const void *owner) noexcept;

std::size_t pending_submit_count_for_tests ();
std::size_t submit_attempt_count_for_tests (const std::string &target);
void reset_async_submit_runtime_for_tests ();

/* Logical Multicast completes a caller task at worker dequeue, while the
 * occupied worker slot is only returned after the job body has run. Saturation
 * tests therefore cannot derive slot state from caller-visible completions and
 * ask the executor directly. */
std::size_t multicast_worker_count_for_tests ();
void wait_for_idle_multicast_executor_for_tests ();

} // namespace zlink::framework::runtime::messaging

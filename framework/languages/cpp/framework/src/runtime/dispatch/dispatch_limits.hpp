/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <chrono>
#include <cstddef>

namespace zlink::framework::runtime::dispatch_limits
{

inline constexpr std::size_t application_mailbox_messages = 1024;
inline constexpr std::size_t application_mailbox_bytes = 64u * 1024u * 1024u;
inline constexpr std::size_t control_mailbox_messages = 128;
inline constexpr std::size_t control_mailbox_bytes = 4u * 1024u * 1024u;

inline constexpr std::size_t receive_batch_messages = 64;
inline constexpr std::size_t receive_batch_bytes = 1u * 1024u * 1024u;
inline constexpr std::chrono::milliseconds receive_batch_time{2};

inline constexpr std::chrono::milliseconds owner_time_budget{10};
inline constexpr std::size_t lifecycle_burst_limit = 8;
inline constexpr std::size_t fixed_work_byte_cost = 256;
inline constexpr std::size_t observer_terminal_retention = 64;

} // namespace zlink::framework::runtime::dispatch_limits

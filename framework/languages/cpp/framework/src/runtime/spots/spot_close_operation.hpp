/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include <zlink/framework/contracts/dispatch/task.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace zlink::framework
{
struct authority_snapshot_t;
}

namespace zlink::framework::runtime::host
{

/* Close step 1 of one authority kind (Spot address messaging §7). An empty
 * `release` means step 1 ended the Close with `result` and left the authority
 * unchanged. Otherwise `Closing` is committed and `release` performs step 4
 * with the same owner and generation fence. Both steps complete when their
 * Location Store operations complete; a caller does not wait for them. */
struct spot_close_commit_t
{
    result_t<bool> result = result_t<bool>::success (false);
    std::function<task_t<bool> ()> release;
    std::function<task_t<authority_snapshot_t> ()> reincarnate;
    std::function<task_t<bool> (const authority_snapshot_t &)> discard_reincarnation;
};
using spot_close_begin_t = std::function<task_t<spot_close_commit_t> ()>;
using spot_close_done_t = std::function<void (result_t<bool>)>;
/* The owner runtime runs a Close for one local User Spot on that Spot's
 * lifecycle lane and reports its result through `done`. */
using user_spot_closer_t = std::function<void (const std::string &,
                                               std::uint64_t,
                                               std::chrono::system_clock::time_point,
                                               spot_close_begin_t,
                                               spot_close_done_t)>;

} // namespace zlink::framework::runtime::host

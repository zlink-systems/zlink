/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include <zlink/framework/contracts/configuration/app.hpp>
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>

namespace zlink::framework::detail
{
struct app_test_access_t
{
    static task_t<bool> startup_publication (app_t &app);
    static const std::atomic_bool &stop_requested (app_t &app);
    static std::uint64_t admission_queue_epoch (const app_t &app);
    static std::optional<std::string> stream_endpoint (app_t &app, const std::string &name);
};
} // namespace zlink::framework::detail

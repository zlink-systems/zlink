/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <zlink/framework/contracts/configuration/app.hpp>
#include <atomic>

namespace zlink::framework::detail
{
struct app_test_access_t
{
    static task_t<bool> startup_publication (app_t &app);
    static const std::atomic_bool &stop_requested (app_t &app);
};
} // namespace zlink::framework::detail

/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../../Shared/store_failure_contracts.hpp"

#include <zlink/framework.hpp>

#include <chrono>
#include <thread>

namespace zlink::framework::e2e::store_failure::provider
{

class provider_lifecycle_control_t
{
  public:
    explicit provider_lifecycle_control_t (zlink::framework::app_t &app) : _app (app) {}

    operation_status_t drain_and_stop (std::chrono::milliseconds deadline)
    {
        const auto result = _app.shutdown (deadline).result ().value ();
        request_stop_after_response ();
        return {
          .status =
            result.outcome == termination_outcome_t::stopped
              ? "stopped"
              : "force_stopped"};
    }

    void request_stop_after_response ()
    {
        auto *app = &_app;
        std::thread ([app] {
            std::this_thread::sleep_for (std::chrono::milliseconds (50));
            app->request_stop ();
        }).detach ();
    }

  private:
    zlink::framework::app_t &_app;
};

} // namespace zlink::framework::e2e::store_failure::provider

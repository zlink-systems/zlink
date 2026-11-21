/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/host_readiness.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{
using namespace zlink::framework;

class readiness_runtime_t final : public zlink::framework::framework_runtime_t
{
  public:
    explicit readiness_runtime_t (framework_runtime_state_t state) : state (state) {}
    framework_runtime_status_t status () const override { return {.state = state}; }
    void reset_capacity_metrics () override { ADD_FAILURE () << "unexpected capacity reset"; }
    listener_status_t listener_status (listener_kind_t, std::string) const override
    {
        ADD_FAILURE () << "unexpected listener status query";
        return {};
    }
    std::vector<http_listener_status_t> http_listener_statuses () const override
    {
        ADD_FAILURE () << "unexpected HTTP listener status query";
        return {};
    }
    std::unique_ptr<runtime_observation_t> observe (
      std::size_t,
      std::function<void (const observed_status_t<framework_runtime_status_t> &)> observer) override
    {
        class observation_t final : public runtime_observation_t
        {
          public:
            explicit observation_t (task_completion_source_t<void> &closed) : closed (closed) {}
            void close () noexcept override { closed.complete (result_t<void>::success ()); }

          private:
            task_completion_source_t<void> &closed;
        };
        observer ({.status = {.state = framework_runtime_state_t::preparing}});
        observer ({.status = status ()});
        return std::make_unique<observation_t> (closed);
    }
    task_completion_source_t<void> closed;

  private:
    framework_runtime_state_t state;
};

TEST (CppFrameworkHostReadiness, ServingCompletesAndClosesObservation)
{
    service_collection_t services;
    auto runtime = std::make_unique<readiness_runtime_t> (framework_runtime_state_t::serving);
    auto closed = runtime->closed.task ();
    services.add_singleton<zlink::framework::framework_runtime_t> (std::move (runtime));
    auto provider = services.build_provider ();
    EXPECT_NO_THROW (zlink::framework::test::wait_for_serving (provider));
    EXPECT_TRUE (closed.await_ready ());
}

TEST (CppFrameworkHostReadiness, StartupFailureCompletesAndClosesObservation)
{
    for (const auto state :
         {framework_runtime_state_t::error, framework_runtime_state_t::stopped}) {
        SCOPED_TRACE (static_cast<int> (state));
        service_collection_t services;
        auto runtime = std::make_unique<readiness_runtime_t> (state);
        auto closed = runtime->closed.task ();
        services.add_singleton<zlink::framework::framework_runtime_t> (std::move (runtime));
        auto provider = services.build_provider ();
        EXPECT_THROW (zlink::framework::test::wait_for_serving (provider), framework_exception_t);
        EXPECT_TRUE (closed.await_ready ());
    }
}
}

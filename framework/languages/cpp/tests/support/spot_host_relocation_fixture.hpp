/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "host_lifecycle_ready_probe.hpp"
#include "host_lifecycle_wait.hpp"
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace zlink::framework::test::spot_host_relocation
{
constexpr auto wait_budget = std::chrono::seconds (5);

class host_t
{
  public:
    ~host_t ()
    {
        app.request_stop ();
        if (_worker.joinable ())
            _worker.join ();
    }

    bool start (std::string program = {})
    {
        auto readiness = std::make_unique<host_lifecycle::ready_host_probe_t> ();
        auto &probe = *readiness;
        app.add_hosted_service (std::move (readiness));
        _worker = std::thread ([this, program = std::move (program)] () mutable {
            if (program.empty ()) {
                app.run (0, nullptr);
                return;
            }
            char *argv[]{program.data (), nullptr};
            app.run (1, argv);
        });
        return probe.wait_ready (wait_budget);
    }

    app_t app = app_t::create ();

  private:
    std::thread _worker;
};

inline bool observe (const std::function<bool ()> &predicate)
{
    return host_lifecycle::wait_until (predicate, wait_budget);
}
} // namespace zlink::framework::test::spot_host_relocation

/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <zlink/framework.hpp>

#include <future>
#include <iostream>
#include <mutex>

int main ()
{
    namespace fw = zlink::framework;
    auto app = fw::app_t::create ();
    app.add_zlink_framework ([] (fw::zlink_framework_options_t &) {});
    auto provider = app.advanced ().services ().build_provider ();
    auto &runtime = provider.get_required<fw::framework_runtime_t> ();
    const auto initial = runtime.status ();
    if (initial.sequence == 0) {
        std::cerr << "initial status has no sequence\n";
        return 1;
    }
    for (int read = 0; read < 64; ++read) {
        const auto unchanged = runtime.status ();
        if (unchanged.sequence != initial.sequence) {
            std::cerr << "stable host payload unexpectedly changed\n";
            return 2;
        }
        if (unchanged != initial) {
            std::cerr << "unchanged payload replaced published status metadata\n";
            return 3;
        }
    }

    runtime.reset_capacity_metrics ();
    const auto reset = runtime.status ();
    if (reset.capacity.measurement_epoch != initial.capacity.measurement_epoch + 1
        || reset.sequence != initial.sequence + 1) {
        std::cerr << "capacity reset did not publish exactly one new status\n";
        return 4;
    }
    for (int read = 0; read < 64; ++read) {
        if (runtime.status () != reset) {
            std::cerr << "reset status was replaced without a payload change\n";
            return 5;
        }
    }

    std::promise<fw::framework_runtime_status_t> first_observed;
    auto observed_future = first_observed.get_future ();
    std::once_flag first_delivery;
    auto observation = runtime.observe (
      1, [&] (const fw::observed_status_t<fw::framework_runtime_status_t> &observed) {
          std::call_once (first_delivery, [&] { first_observed.set_value (observed.status); });
      });
    const auto observed = observed_future.get ();
    observation->close ();
    if (observed != reset || runtime.status () != observed) {
        std::cerr << "observer and query did not share the published status\n";
        return 6;
    }
    std::cout << "stable reads, capacity reset, and observer publication passed\n";
}

/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/host_lifecycle_blocking_stop.hpp"
#include "host_lifecycle_tests.hpp"

#include <zlink/json.hpp>

#include <zlink/framework.hpp>
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/location_repository.hpp"
#include "runtime/locations/location_records.hpp"
#include "runtime/spots/spot_runtime.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <functional>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>


namespace zlink::framework::test::host_lifecycle
{
int verify_shutdown_termination ()
{
    auto app = zlink::framework::app_t::create ();
    if (app.runtime_state () != zlink::framework::framework_runtime_state_t::preparing) {
        std::cerr << "new app must begin in Preparing\n";
        return EXIT_FAILURE;
    }

    bool planned_target_rejected = false;
    try {
        (void) app.relocate ({.mode = zlink::framework::relocation_mode_t::planned_maintenance,
                              .target_application_version = 2});
    }
    catch (const std::invalid_argument &) {
        planned_target_rejected = true;
    }
    bool rolling_target_required = false;
    try {
        (void) app.relocate ({.mode = zlink::framework::relocation_mode_t::rolling_update});
    }
    catch (const std::invalid_argument &) {
        rolling_target_required = true;
    }
    if (!planned_target_rejected || !rolling_target_required) {
        std::cerr << "Relocate mode must validate its target version option\n";
        return EXIT_FAILURE;
    }

    const auto relocation =
      app.relocate ({.mode = zlink::framework::relocation_mode_t::planned_maintenance})
        .result ()
        .value ();
    if (relocation.outcome != zlink::framework::relocation_outcome_t::blocked
        || relocation.reason != zlink::framework::relocation_reason_t::runtime_not_ready) {
        std::cerr << "Relocate before Serving must return Blocked/RuntimeNotReady\n";
        return EXIT_FAILURE;
    }

    const auto shutdown = app.shutdown (std::chrono::seconds (1)).result ().value ();
    const auto shutdown_state = app.runtime_state ();
    if (shutdown.outcome != zlink::framework::termination_outcome_t::stopped
        || shutdown.reason != zlink::framework::termination_reason_t::none
        || shutdown_state != zlink::framework::framework_runtime_state_t::stopped) {
        std::cerr << "Shutdown must complete the shared termination operation"
                  << " outcome=" << static_cast<int> (shutdown.outcome)
                  << " reason=" << static_cast<int> (shutdown.reason)
                  << " state=" << static_cast<int> (shutdown_state) << '\n';
        return EXIT_FAILURE;
    }
    const auto after_shutdown =
      app.relocate ({.mode = zlink::framework::relocation_mode_t::planned_maintenance})
        .result ()
        .value ();
    if (after_shutdown.outcome != zlink::framework::relocation_outcome_t::blocked
        || after_shutdown.reason != zlink::framework::relocation_reason_t::runtime_not_ready) {
        std::cerr << "Relocate after Shutdown must report RuntimeNotReady\n";
        return EXIT_FAILURE;
    }

    auto running = zlink::framework::app_t::create ();
    auto service = std::make_unique<blocking_stop_service_t> ();
    auto *service_view = service.get ();
    running.add_hosted_service (std::move (service));
    char program[] = "termination-facade";
    char *arguments[] = {program, nullptr};
    int exit_code = -1;
    std::thread run_thread ([&] { exit_code = running.run (1, arguments); });
    service_view->wait_started ();
    const auto serving_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (1);
    while (!running.is_ready () && std::chrono::steady_clock::now () < serving_deadline)
        std::this_thread::yield ();

    const auto unavailable_relocation =
      running
        .relocate ({.mode = zlink::framework::relocation_mode_t::planned_maintenance,
                    .deadline = std::chrono::seconds (2)})
        .result ()
        .value ();
    // Spec 05 §5.1: an empty host needs neither a target nor a Store descriptor.
    if (unavailable_relocation.outcome != zlink::framework::relocation_outcome_t::relocated
        || unavailable_relocation.reason != zlink::framework::relocation_reason_t::none
        || running.runtime_state () != zlink::framework::framework_runtime_state_t::relocated) {
        std::cerr << "Empty host without a Store must finish Relocated/None\n";
        running.stop ();
        service_view->release ();
        run_thread.join ();
        return EXIT_FAILURE;
    }

    auto shared_shutdown = running.shutdown (std::chrono::seconds (2));
    service_view->wait_stop_entered ();
    if (shared_shutdown.await_ready ()) {
        std::cerr << "Shutdown must wait for hosted-service teardown\n";
        service_view->release ();
        run_thread.join ();
        return EXIT_FAILURE;
    }

    std::stop_source cancelled_source;
    cancelled_source.request_stop ();
    auto cancelled_waiter =
      running.shutdown (std::chrono::seconds (2), cancelled_source.get_token ());
    const auto &cancelled = cancelled_waiter.result ();
    bool waiter_cancelled = false;
    try {
        cancelled.value ();
    }
    catch (const std::system_error &error) {
        waiter_cancelled = error.code () == std::make_error_code (std::errc::operation_canceled);
    }
    if (!waiter_cancelled) {
        std::cerr << "wait cancellation must cancel only the joining waiter\n";
        service_view->release ();
        run_thread.join ();
        return EXIT_FAILURE;
    }

    service_view->release ();
    const auto shared_result = shared_shutdown.result ().value ();
    run_thread.join ();
    if (shared_result.outcome != zlink::framework::termination_outcome_t::stopped
        || exit_code != 0) {
        std::cerr << "shared Shutdown must survive waiter cancellation\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
}

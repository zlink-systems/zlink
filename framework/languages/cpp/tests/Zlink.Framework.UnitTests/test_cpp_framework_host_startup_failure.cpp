/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework.hpp>
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/utils/poll_interval_wait.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>

int main ()
{
    using namespace zlink::framework;
    constexpr auto observation_budget = std::chrono::seconds (3);
    constexpr auto observation_interval = std::chrono::milliseconds (1);
    auto store = std::make_shared<runtime::in_memory_location_store_t> ();
    auto owner = app_t::create ();
    auto conflict = app_t::create ();
    for (auto *app : {&owner, &conflict}) {
        auto &options = app->add_zlink_framework ();
        options.add_location_store (store);
        options.add_route_mesh ("startup-conflict")
          .listen ("tcp://127.0.0.1:0")
          .set_routing_id (zlink::routing_id_t::from ("startup-conflict"));
    }
    char program[] = "startup-conflict";
    char *arguments[] = {program, nullptr};
    int owner_result = -1;
    std::thread owner_thread ([&] { owner_result = owner.run (1, arguments); });
    const auto owner_deadline = std::chrono::steady_clock::now () + observation_budget;
    while (!owner.is_ready () && std::chrono::steady_clock::now () < owner_deadline)
        runtime::wait_poll_interval (observation_interval);
    if (!owner.is_ready ()) {
        owner.request_stop ();
        owner_thread.join ();
        std::cerr << "owner must finish startup before conflict is submitted\n";
        return EXIT_FAILURE;
    }

    std::atomic_bool completed{false};
    int conflict_result = -1;
    std::thread conflict_thread ([&] {
        conflict_result = conflict.run (1, arguments);
        completed.store (true, std::memory_order_release);
    });
    const auto conflict_deadline = std::chrono::steady_clock::now () + observation_budget;
    while (!completed.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < conflict_deadline)
        runtime::wait_poll_interval (observation_interval);
    const bool failed_startup = completed.load (std::memory_order_acquire);
    conflict.request_stop ();
    conflict_thread.join ();
    owner.request_stop ();
    owner_thread.join ();
    if (!failed_startup || conflict_result == 0 || owner_result != 0) {
        std::cerr << "RoutingId conflict must fail host startup\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

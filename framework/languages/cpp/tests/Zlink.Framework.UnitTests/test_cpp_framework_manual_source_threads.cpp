/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "runtime/client_server/raw_client_server_owner.hpp"
#include <zlink/Contracts/Core/context.hpp>
#include <filesystem>
#include <iostream>
#include <type_traits>
namespace fw = zlink::framework;
using owner_t = fw::runtime::client_server::raw_client_server_server_t;
using options_t = fw::runtime::client_server::raw_client_server_server_options_t;
std::size_t threads ()
{
    std::size_t count = 0;
    for (const auto &entry : std::filesystem::directory_iterator ("/proc/self/task")) {
        (void) entry;
        ++count;
    }
    return count;
}
template <class Owner>
auto make_owner (options_t options,
                 std::shared_ptr<zlink::context_t> context,
                 fw::runtime::offload_executor_t &executor)
{
    if constexpr (std::is_constructible_v<Owner, options_t, std::shared_ptr<zlink::context_t>,
                                          fw::runtime::offload_executor_t *>)
        return std::make_unique<Owner> (std::move (options), std::move (context), &executor);
    else
        return std::make_unique<Owner> (std::move (options), std::move (context));
}
int main ()
{
    fw::runtime::offload_executor_t infrastructure (1, "thread-probe-infrastructure");
    auto context = std::make_shared<zlink::context_t> ();
    fw::runtime::protocol::client_server_server_admission_t descriptor;
    descriptor.channel_name = "thread-probe";
    descriptor.server_routing_id = {'s'};
    descriptor.descriptor_revision = 1;
    options_t options{descriptor};
    options.runtime_failures = std::make_shared<fw::runtime::runtime_failure_collector_t> ();
    const auto before = threads ();
    auto owner = make_owner<owner_t> (std::move (options), context, infrastructure);
    const auto after = threads ();
    std::cout << "raw source+registry expected added threads=0 actual=" << after - before << '\n';
    if (after != before)
        return 1;
    // next_activity enters the registry lane from the raw owner lane. Both must finish.
    if (owner->next_liveness_activity_task ().result ().value ())
        return 2;
    auto changed = owner->descriptor_task ().result ().value ();
    ++changed.descriptor_revision;
    changed.weight = 37;
    owner->update_descriptor_task (changed).result ().value ();
    if (owner->descriptor_task ().result ().value ().weight != 37)
        return 3;
    (void) owner->tick_liveness (fw::runtime::mesh::service_liveness_registry_t::clock_t::now ())
      .result ()
      .value ();
    if (threads () != before)
        return 4;
    owner.reset ();
    std::cout << "nested state lane/descriptor/tick/close completed; added threads=0\n";
}

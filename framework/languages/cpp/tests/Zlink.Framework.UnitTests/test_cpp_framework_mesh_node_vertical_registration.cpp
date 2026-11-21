/* SPDX-License-Identifier: Elastic-2.0 */
#include "../support/mesh_node_vertical_cases.hpp"
#include <zlink.hpp>
#include <atomic>
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/dispatch/completion_turn.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/execution/infrastructure_wait_guard.hpp"
#include "runtime/mesh/mesh_node_host_service.hpp"
#include "runtime/mesh/route_mesh_runtime_options_service.hpp"
#include "runtime/mesh/route_mesh_runtime_service.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "../support/loopback_tcp_endpoint.hpp"
#include <zlink/framework/contracts/configuration/zlink_builder.hpp>
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <latch>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#include "../support/mesh_node_vertical_transport_fixture.hpp"

using namespace framework_mesh_node_vertical_fixture;

using namespace std::chrono_literals;

namespace
{
static_assert (std::is_same_v<decltype (std::declval<zlink::framework::zlink_builder_t &> ()
                                          .add_route_mesh (std::declval<std::string> ())),
                              zlink::framework::mesh_node_builder_t>);

static_assert (std::is_same_v<decltype (std::declval<zlink::framework::mesh_node_builder_t &> ()
                                          .channel_name (std::declval<std::string> ())),
                              zlink::framework::mesh_channel_builder_t>);

static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::mesh_channel_builder_t &> ().client ()),
                 zlink::framework::mesh_channel_client_builder_t>);

static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::mesh_channel_builder_t &> ().server ()),
                 zlink::framework::mesh_channel_server_builder_t>);

struct contract_actor_t final : zlink::framework::actor_t
{
    explicit contract_actor_t (zlink::framework::actor_context_t context) :
        value (std::move (context))
    {
    }
    zlink::framework::actor_context_t &context () noexcept override { return value; }
    const zlink::framework::actor_context_t &context () const noexcept override { return value; }
    zlink::framework::actor_context_t value;
};

struct contract_actor_factory_t final : zlink::framework::actor_factory_t<contract_actor_t>
{
    zlink::framework::task_t<std::shared_ptr<contract_actor_t>>
    create (zlink::framework::actor_context_t context, std::stop_token) override
    {
        co_return std::make_shared<contract_actor_t> (std::move (context));
    }
};

struct contract_entry_spot_t : zlink::framework::entry_spot_t<contract_actor_t>
{
};

struct contract_spot_t : zlink::framework::spot_t<contract_actor_t>
{
};

static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::mesh_node_builder_t &> ()
                             .add_entry_spot<contract_entry_spot_t> (
                               std::declval<std::function<std::shared_ptr<contract_entry_spot_t> (
                                 zlink::framework::entry_spot_context_t)>> ())),
                 zlink::framework::mesh_node_builder_t &>);

static_assert (
  std::is_same_v<
    decltype (std::declval<zlink::framework::mesh_node_builder_t &> ()
                .add_spot_factory<contract_spot_t> (
                  std::declval<std::string> (),
                  std::declval<std::function<
                    std::shared_ptr<contract_spot_t> (zlink::framework::spot_context_t)>> (),
                  std::declval<std::function<
                    void (zlink::framework::user_spot_factory_builder_t<contract_spot_t> &)>> ())),
    zlink::framework::mesh_node_builder_t &>);

static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::mesh_node_builder_t &> ()
                             .add_actor_factory<contract_actor_t, contract_actor_factory_t> (
                               std::declval<std::string> (),
                               std::declval<std::shared_ptr<contract_actor_factory_t>> (),
                               [] (auto &factory) { factory.disable_relocation (); })),
                 zlink::framework::mesh_node_builder_t &>);
} // namespace

namespace framework_mesh_node_vertical_tests
{
void verify_object_client_registration_boundary ()
{
    auto client_state = make_node ("tcp://127.0.0.1:*", "object-client");
    client_state->object_role = zlink::framework::object_role_t::client;
    client_state->channels.clear ();
    zlink::framework::detail::mesh_node_runtime_t client (client_state);
    client.start ().result ().value ();
    const auto descriptor = client.native_node ().transport ().topology ().local_descriptor ();
    assert (descriptor.object_role
            == zlink::framework::runtime::mesh::service_object_role_t::client);
    assert (descriptor.channels.empty ());
    client.stop ();

    auto invalid_state = make_node ("tcp://127.0.0.1:*", "invalid-object-client");
    invalid_state->object_role = zlink::framework::object_role_t::client;
    invalid_state->has_node_direct_handler = true;
    zlink::framework::detail::mesh_node_runtime_t invalid (invalid_state);
    bool rejected = false;
    try {
        invalid.start ().result ().value ();
    }
    catch (const zlink::framework::framework_exception_t &) {
        rejected = true;
    }
    assert (rejected);
}

void verify_unselected_object_role_defaults_to_none ()
{
    auto state = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::mesh_node_builder_state_t> ("unselected-object-role");
    zlink::framework::detail::mesh_node_runtime_t runtime (state);

    assert (state->object_role == zlink::framework::object_role_t::none);
    assert (runtime.object_role () == zlink::framework::object_role_t::none);
    assert (state->spot_state->spot_factories.empty ());
    assert (!state->spot_state->snapshot.entry_spot_name.has_value ());
}
} // namespace framework_mesh_node_vertical_tests

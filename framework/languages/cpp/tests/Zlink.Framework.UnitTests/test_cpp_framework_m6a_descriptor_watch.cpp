/* SPDX-License-Identifier: Elastic-2.0 */

#include "m6a_runtime_tests.hpp"
#include "../support/m6a_mesh_fixture.hpp"
#include "runtime/locations/service_descriptor_registry.hpp"

#include <cassert>
#include <optional>
#include <utility>
#include <vector>

namespace zlink::framework::test::m6a
{
namespace locations = runtime::locations;

void verify_location_descriptor_cas_snapshot_and_watch ()
{
    locations::service_descriptor_registry_t registry;
    std::vector<locations::service_descriptor_event_t> events;
    const auto watch =
      registry.watch ({locations::service_descriptor_kind_t::client_server, "alpha"},
                      [&events] (locations::service_descriptor_event_t event) {
                          events.push_back (std::move (event));
                      });
    locations::service_descriptor_record_t record{
      {locations::service_descriptor_kind_t::client_server, "alpha", bytes ("server-a")},
      11,
      1,
      "tcp://127.0.0.1:7001",
      "security-a",
      1024 * 1024,
      mesh::service_node_state_t::serving,
      100,
      "owner-a",
      17};
    assert (registry.publish (record, std::nullopt)
            == locations::service_descriptor_publish_status_t::inserted);
    auto stale = record;
    stale.descriptor_revision = 2;
    assert (registry.publish (stale, 9)
            == locations::service_descriptor_publish_status_t::conflict);
    auto immutable_mutation = record;
    immutable_mutation.descriptor_revision = 2;
    immutable_mutation.endpoint = "tcp://127.0.0.1:7002";
    assert (registry.publish (immutable_mutation, 1)
            == locations::service_descriptor_publish_status_t::conflict);
    auto updated = record;
    updated.descriptor_revision = 2;
    updated.weight = 0;
    updated.state = mesh::service_node_state_t::draining;
    assert (registry.publish (updated, 1)
            == locations::service_descriptor_publish_status_t::updated);
    const auto snapshot =
      registry.snapshot ({locations::service_descriptor_kind_t::client_server, "alpha"});
    assert (snapshot.change_stamp == 2);
    assert (snapshot.records == std::vector{updated});
    assert (!registry.remove (updated.key, 1, updated.owner_id, updated.owner_lease_generation));
    assert (registry.remove (updated.key, 2, updated.owner_id, updated.owner_lease_generation));
    assert (events.size () == 3);
    assert (events.back ().change == locations::service_descriptor_change_t::removed);
    assert (events.back ().change_stamp == 3);
    assert (registry.unwatch (watch));
    assert (!registry.unwatch (watch));
}
}

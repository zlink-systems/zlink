/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "runtime/locations/live_location_reader.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include <zlink/framework/contracts/configuration/services.hpp>
#include <memory>

namespace framework_mesh_node_vertical_fixture
{
void register_mesh_location_resolvers (zlink::framework::service_collection_t &services);

inline void register_mesh_location_resolvers (zlink::framework::service_collection_t &services)
{
    services.add_factory<zlink::framework::runtime::live_location_reader_t> (
      [] (zlink::framework::service_provider_t &provider) {
          return std::make_unique<zlink::framework::runtime::live_location_reader_t> (
            provider.get_required<zlink::framework::location_repository_t> ());
      },
      zlink::framework::service_lifetime_t::singleton);
    services.add_factory<zlink::framework::runtime::store_location_resolvers_t> (
      [] (zlink::framework::service_provider_t &provider) {
          return std::make_unique<zlink::framework::runtime::store_location_resolvers_t> (
            provider.get_required<zlink::framework::location_repository_t> ());
      },
      zlink::framework::service_lifetime_t::singleton);
    services.add_factory<zlink::framework::runtime::spot_address_resolver_t> (
      [] (zlink::framework::service_provider_t &provider) {
          return std::shared_ptr<zlink::framework::runtime::spot_address_resolver_t> (
            &provider.get_required<zlink::framework::runtime::store_location_resolvers_t> (),
            [] (zlink::framework::runtime::spot_address_resolver_t *) noexcept {});
      },
      zlink::framework::service_lifetime_t::singleton);
    services.add_factory<zlink::framework::runtime::actor_address_resolver_t> (
      [] (zlink::framework::service_provider_t &provider) {
          return std::shared_ptr<zlink::framework::runtime::actor_address_resolver_t> (
            &provider.get_required<zlink::framework::runtime::store_location_resolvers_t> (),
            [] (zlink::framework::runtime::actor_address_resolver_t *) noexcept {});
      },
      zlink::framework::service_lifetime_t::singleton);
}
} // namespace framework_mesh_node_vertical_fixture

/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/locations/in_memory_location_store.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/locations/location_runtime.hpp"

#include <gtest/gtest.h>

namespace
{

using zlink::framework::location_options_t;
using zlink::framework::runtime::in_memory_location_repository_t;
using zlink::framework::runtime::location_runtime_t;

TEST (ZLinkFrameworkLocationRuntime, ClaimsAndReleasesOwnerLease)
{
    in_memory_location_repository_t store;
    location_runtime_t runtime (
      store,
      location_options_t{.owner_lease_renew_interval = std::chrono::milliseconds (5),
                         .owner_lease_ttl = std::chrono::seconds (15)},
      "owner-a");

    runtime.start (zlink::routing_id_t::from ("node-a"));
    EXPECT_TRUE (runtime.owner_lease_healthy ());
    EXPECT_TRUE (
      std::holds_alternative<
        zlink::framework::owner_lease_found_t> (
        store.read_owner_lease ("owner-a")
          .result ()
          .value ()));

    runtime.stop ();
    EXPECT_TRUE (
      std::holds_alternative<
        zlink::framework::owner_lease_missing_t> (
        store.read_owner_lease ("owner-a")
          .result ()
          .value ()));
}

} // namespace

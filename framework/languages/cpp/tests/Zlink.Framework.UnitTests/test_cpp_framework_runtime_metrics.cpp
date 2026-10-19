/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "../support/runtime_failure_fixture.hpp"
#include "../support/runtime_metric_contract.hpp"
#include "runtime/diagnostics/runtime_metrics.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include <zlink/framework.hpp>
#include <cmath>
#include <iostream>
#include <thread>

using namespace zlink::framework;
using namespace zlink::framework::runtime;
using namespace std::chrono_literals;
namespace
{
void require (bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error (message);
}
using tags_t = std::map<std::string, std::string>;
std::optional<double>
point (metric_test::provider_t &provider, const std::string &name, const tags_t &tags)
{
    const auto data = provider.collect ();
    metric_test::validate_metric_contract (data);
    for (const auto &metric : data) {
        if (metric.instrument_descriptor.name_ != name)
            continue;
        for (const auto &point : metric.point_data_attr_) {
            tags_t actual;
            for (const auto &[key, value] : point.attributes)
                actual.emplace (key, opentelemetry::nostd::get<std::string> (value));
            if (actual != tags)
                continue;
            if (const auto *gauge =
                  opentelemetry::nostd::get_if<metric_test::sdk::LastValuePointData> (
                    &point.point_data))
                return opentelemetry::nostd::get<double> (gauge->value_);
            if (const auto *sum =
                  opentelemetry::nostd::get_if<metric_test::sdk::SumPointData> (&point.point_data))
                return opentelemetry::nostd::get<double> (sum->value_);
            if (const auto *histogram =
                  opentelemetry::nostd::get_if<metric_test::sdk::HistogramPointData> (
                    &point.point_data))
                return static_cast<double> (histogram->count_);
        }
    }
    return std::nullopt;
}
void require_one_current_host_state (metric_test::provider_t &provider)
{
    double total = 0;
    const auto data = provider.collect ();
    metric_test::validate_metric_contract (data);
    for (const auto &metric : data) {
        if (metric.instrument_descriptor.name_ != "zlink.host.state") continue;
        for (const auto &point : metric.point_data_attr_) {
            const auto &gauge = opentelemetry::nostd::get<metric_test::sdk::LastValuePointData> (point.point_data);
            total += opentelemetry::nostd::get<double> (gauge.value_);
        }
    }
    require (total == 1, "host has more than one current state");
}
void host_lifecycle ()
{
    metric_test::provider_t provider;
    auto app = app_t::create ();
    require (point (provider, "zlink.host.state", {{"state", "preparing"}}) == 1,
             "host Preparing state missing");
    const auto relocation = app.relocate ({}).result ().value ();
    require (relocation.outcome == relocation_outcome_t::blocked, "unstarted relocation result");
    require (point (provider, "zlink.host.relocation.blocked",
                    {{"mode", "planned_maintenance"}, {"reason", "runtime_not_ready"}})
               == 1,
             "host relocation blocker not recorded exactly once");
    const auto shutdown = app.shutdown (1s).result ().value ();
    require (shutdown.outcome == termination_outcome_t::stopped, "host shutdown failed");
    require (point (provider, "zlink.host.shutdown.duration", {{"outcome", "stopped"}}) == 1,
             "host shutdown duration missing");
    (void) app.shutdown (1s).result ().value ();
    for (unsigned i = 0; i != 2; ++i) {
        require (point (provider, "zlink.host.state", {{"state", "stopped"}}) == 1,
                 "Stopped state was not retained");
        require_one_current_host_state (provider);
        require (point (provider, "zlink.host.state", {{"state", "preparing"}}).value_or (0) == 0,
                 "departed Preparing state remained at one");
        require (point (provider, "zlink.host.shutdown.duration", {{"outcome", "stopped"}}) == 1,
                 "joined shutdown double counted");
    }
}

void serving_relocated_stopped ()
{
    metric_test::provider_t provider;
    auto app = app_t::create ();
    struct runner_t
    {
        app_t &app;
        std::thread thread;
        ~runner_t ()
        {
            app.stop ();
            if (thread.joinable ())
                thread.join ();
        }
    } runner{app, std::thread ([&] {
                 char program[] = "runtime-metrics";
                 char *arguments[] = {program, nullptr};
                 (void) app.run (1, arguments);
             })};
    const auto deadline = std::chrono::steady_clock::now () + 2s;
    while (!app.is_ready () && std::chrono::steady_clock::now () < deadline)
        std::this_thread::yield ();
    require (app.is_ready (), "host did not enter Serving");
    require (point (provider, "zlink.host.state", {{"state", "serving"}}) == 1,
             "Serving state missing");
    const auto relocated = app.relocate ({}).result ().value ();
    require (relocated.outcome == relocation_outcome_t::relocated, "empty host relocation failed");
    require (point (provider, "zlink.host.state", {{"state", "relocated"}}) == 1,
             "Relocated state missing");
    require_one_current_host_state (provider);
    require (point (provider, "zlink.host.state", {{"state", "serving"}}).value_or (0) == 0,
             "departed Serving state remained at one");
    require (point (provider, "zlink.host.relocation.duration",
                    {{"mode", "planned_maintenance"}, {"outcome", "relocated"}})
               == 1,
             "successful host relocation duration not recorded once");
    (void) app.shutdown (1s).result ().value ();
    require (point (provider, "zlink.host.state", {{"state", "stopped"}}) == 1,
             "Stopped state missing after active host shutdown");
}

class fault_store_t final : public location_store_t
{
  public:
    in_memory_location_store_t inner;
    bool fail_capacity = false;
    unsigned capacity_reads = 0;
    task_t<store_read_result_t> read (store_key_t key) override
    {
        if (key.value.find (":capacity:") != std::string::npos) {
            ++capacity_reads;
            if (fail_capacity)
                throw framework_exception_t (framework_error_kind_t::unavailable,
                                             "injected capacity read failure");
        }
        co_return co_await inner.read (std::move (key));
    }
    task_t<store_write_result_t> write (store_write_request_t request) override
    {
        return inner.write (std::move (request));
    }
    task_t<store_scan_result_t> scan (store_scan_request_t request) override
    {
        return inner.scan (std::move (request));
    }
};
std::vector<std::byte> bytes (std::string_view text)
{
    return {reinterpret_cast<const std::byte *> (text.data ()),
            reinterpret_cast<const std::byte *> (text.data () + text.size ())};
}
void capacity_transitions ()
{
    metric_test::provider_t provider;
    fault_store_t store;
    auto monitoring = test::runtime_failure_monitoring ();
    provider_location_repository_t local (store, monitoring);
    // A second host writes the same Store, with its own repository cache.
    provider_location_repository_t remote (store);
    const auto owner = std::get<owner_lease_claimed_t> (
                         local.claim_owner_lease ("metrics-owner", 30s).result ().value ())
                         .token;
    mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = "capacity-mesh";
    descriptor.rid = zlink::routing_id_t::from (std::string ("capacity-local"));
    descriptor.lifecycle_generation = descriptor.descriptor_revision = 1;
    descriptor.endpoint = "tcp://127.0.0.1:7001";
    descriptor.owner_id = owner.owner_id;
    descriptor.lease_generation = owner.lease_generation;
    descriptor.object_role = object_role_t::server;
    descriptor.state = framework_runtime_state_t::serving;
    descriptor.object_capabilities.push_back (
      {placement_object_kind_t::actor, "player", maintenance_policy_kind_t::recreate, false, 0});
    descriptor.capacity.actors.limit = 10;
    descriptor.capacity.spots.limit = 10;
    descriptor.object_capabilities.push_back (
      {placement_object_kind_t::user_spot, "room", maintenance_policy_kind_t::recreate, false, 0});
    descriptor.capacity.spot_types.push_back (
      {placement_object_kind_t::user_spot, "room", {0, 0, 7}});
    require (local.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status
               == location_write_status_t::stored,
             "descriptor publication failed");
    const tags_t labels{{"mesh_name", "capacity-mesh"}, {"capacity_scope", "actor"}};
    require (!point (provider, "zlink.object.capacity.active", labels),
             "unconfirmed capacity emitted");
    const auto reads_before_collection = store.capacity_reads;
    (void) provider.collect ();
    require (store.capacity_reads == reads_before_collection, "observable performed Store read");
    local.refresh_capacity_metrics (descriptor.mesh_name, descriptor.rid).result ().value ();
    require (store.capacity_reads == reads_before_collection + 1, "poll read more than once");
    require (point (provider, "zlink.object.capacity.active", labels) == 0,
             "empty capacity not confirmed");
    require (point (provider, "zlink.object.capacity.limit", labels) == 10,
             "capacity limit missing");
    object_reserve_request_t request;
    request.key = {placement_object_kind_t::actor, "metrics-actor"};
    request.intent.stable_type = "player";
    request.target = {descriptor.mesh_name, node_rid_t::from_string (descriptor.rid.to_string ()),
                      1, owner};
    request.creating_payload = bytes ("creating");
    request.capacity_bundle.actor_slots = 1;
    const auto reservation =
      std::get<object_reserved_t> (local.reserve (request).result ().value ());
    require (point (provider, "zlink.object.capacity.reserved", labels) == 1,
             "successful reservation CAS not observed");
    actor_authority_payload_t payload;
    payload.state = actor_authority_state_t::ready;
    payload.stable_type = "player";
    payload.actor_id = "metrics-actor";
    payload.current_spot_id = "entry";
    payload.current_spot_generation = 1;
    payload.current_spot_kind = actor_authority_spot_kind_t::entry;
    payload.owner_id = owner.owner_id;
    payload.owner_lease_generation = owner.lease_generation;
    payload.mesh_name = descriptor.mesh_name;
    payload.node_rid = request.target.node_rid;
    payload.node_generation = 1;
    require (
      std::holds_alternative<object_committed_t> (
        local.commit ({request.key, reservation.fence, encode_actor_authority_payload (payload)})
          .result ()
          .value ()),
      "capacity commit failed");
    require (point (provider, "zlink.object.capacity.active", labels) == 1,
             "commit active capacity missing");
    require (point (provider, "zlink.object.capacity.reserved", labels) == 0,
             "commit reservation retained");
    request.key.global_id = "metrics-remote-actor";
    (void) std::get<object_reserved_t> (remote.reserve (request).result ().value ());
    require (point (provider, "zlink.object.capacity.reserved", labels) == 0,
             "remote host changed local cache without poll");
    request.key = {placement_object_kind_t::user_spot, "metrics-remote-room"};
    request.intent.stable_type = "room";
    request.capacity_bundle = {
      0, 1, spot_type_capacity_delta_t{placement_object_kind_t::user_spot, "room", 1}};
    (void) std::get<object_reserved_t> (remote.reserve (request).result ().value ());
    local.refresh_capacity_metrics (descriptor.mesh_name, descriptor.rid).result ().value ();
    require (point (provider, "zlink.object.capacity.reserved", labels) == 1,
             "next poll missed remote reservation");
    const tags_t typed{
      {"mesh_name", "capacity-mesh"}, {"spot_kind", "user"}, {"stable_type", "room"}};
    require (point (provider, "zlink.spot.type.capacity.active", typed) == 0,
             "typed active population incorrect");
    require (point (provider, "zlink.spot.type.capacity.reserved", typed) == 1,
             "typed reservation missing");
    require (point (provider, "zlink.spot.type.capacity.limit", typed) == 7, "typed limit missing");
    store.fail_capacity = true;
    const auto failed =
      local.refresh_capacity_metrics (descriptor.mesh_name, descriptor.rid).result ();
    require (!failed, "injected read failure was accepted");
    require (point (provider, "zlink.object.capacity.reserved", labels) == 1,
             "read failure erased last confirmed value");
    require (point (provider, "zlink.location.store.errors", {{"operation", "read"}}) == 1,
             "failed read was not counted exactly once");
}
class unavailable_meter_provider_t final : public opentelemetry::metrics::MeterProvider
{
  public:
#if OPENTELEMETRY_ABI_VERSION_NO >= 2
    opentelemetry::nostd::shared_ptr<opentelemetry::metrics::Meter>
    GetMeter (opentelemetry::nostd::string_view,
              opentelemetry::nostd::string_view,
              opentelemetry::nostd::string_view,
              const opentelemetry::common::KeyValueIterable *) noexcept override
    {
        return {};
    }
    void RemoveMeter (opentelemetry::nostd::string_view,
                      opentelemetry::nostd::string_view,
                      opentelemetry::nostd::string_view) noexcept override
    {
    }
#else
    opentelemetry::nostd::shared_ptr<opentelemetry::metrics::Meter>
    GetMeter (opentelemetry::nostd::string_view,
              opentelemetry::nostd::string_view,
              opentelemetry::nostd::string_view) noexcept override
    {
        return {};
    }
#endif
};
void provider_failure_preserves_shutdown ()
{
    struct provider_scope_t
    {
        opentelemetry::nostd::shared_ptr<opentelemetry::metrics::MeterProvider> previous =
          opentelemetry::metrics::Provider::GetMeterProvider ();
        ~provider_scope_t () { opentelemetry::metrics::Provider::SetMeterProvider (previous); }
    } scope;
    opentelemetry::metrics::Provider::SetMeterProvider (
      opentelemetry::nostd::shared_ptr<opentelemetry::metrics::MeterProvider> (
        std::static_pointer_cast<opentelemetry::metrics::MeterProvider> (
          std::make_shared<unavailable_meter_provider_t> ())));
    auto app = app_t::create ();
    require (app.shutdown (1s).result ().value ().outcome == termination_outcome_t::stopped,
             "unavailable meter changed host Shutdown result");
}

void contract_rejects_real_bad_label ()
{
    metric_test::provider_t provider;
    runtime_metrics_t metrics (test::runtime_failure_monitoring ());
    metrics.counter ("zlink.location.store.errors", "{error}", 1,
                     {{"operation", "not_an_operation"}});
    bool rejected = false;
    try {
        metric_test::validate_metric_contract (provider.collect ());
    }
    catch (const std::runtime_error &error) {
        rejected = std::string (error.what ()).find ("not_an_operation") != std::string::npos;
    }
    require (rejected, "contract accepted real invalid operation label");
    std::cout << "negative contract: rejected operation=not_an_operation\n";
}
} // namespace
int main ()
{
    try {
        host_lifecycle ();
        serving_relocated_stopped ();
        capacity_transitions ();
        provider_failure_preserves_shutdown ();
        contract_rejects_real_bad_label ();
    }
    catch (const std::exception &error) {
        std::cerr << error.what () << '\n';
        return 1;
    }
    std::cout << "host lifecycle, capacity transitions, negative metric contract passed\n";
}

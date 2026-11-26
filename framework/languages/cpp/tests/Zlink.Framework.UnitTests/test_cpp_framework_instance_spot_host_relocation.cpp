/* SPDX-License-Identifier: Elastic-2.0 */
#include "../support/instance_spot_activation_messages.hpp"
#include "../support/spot_host_relocation_fixture.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/relocation_authority_payload.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <zlink/framework.hpp>
#include <gtest/gtest.h>
#include <atomic>

namespace
{
namespace fw = zlink::framework;
namespace stateful = fw::runtime::stateful;
using namespace framework_instance_spot_activation_fixture;
using namespace std::chrono_literals;

constexpr auto mesh_name = "instance-host-relocation";
constexpr auto stable_type = "player-quest";
constexpr auto source_rid = "instance-source";
constexpr auto target_rid = "instance-target";
constexpr auto spot_id = "instance-alice";
using fw::test::spot_host_relocation::host_t;
using fw::test::spot_host_relocation::observe;
using fw::test::spot_host_relocation::wait_budget;

class retained_address_resolver_t final : public fw::runtime::spot_address_resolver_t
{
  public:
    explicit retained_address_resolver_t (fw::runtime::spot_address_t address) :
        _address (std::move (address))
    {
    }

    fw::task_t<std::optional<fw::runtime::spot_address_t>>
    resolve_spot_address (std::string, std::string id) override
    {
        if (id == _address.spot_id)
            co_return _address;
        co_return std::nullopt;
    }

    void invalidate_spot_address (std::string_view) override {}
    void invalidate_all_routes_after_store_recovery () override {}

  private:
    const fw::runtime::spot_address_t _address;
};

class instance_t final : public fw::instance_spot_t
{
  public:
    instance_t (fw::instance_spot_context_t context, std::atomic<unsigned> &restores) :
        restores (restores), _context (std::move (context))
    {
    }
    fw::instance_spot_context_t &context () noexcept override { return _context; }
    const fw::instance_spot_context_t &context () const noexcept override { return _context; }

    void configure () override
    {
        _context.handlers ().add_handler<&instance_t::add> ();
        _context.handlers ().add_handler<&instance_t::read> ();
    }

    fw::task_t<void> on_closing (const fw::spot_closing_context_t &, std::stop_token) override
    {
        if (closing)
            co_await closing ();
    }

    std::function<fw::task_t<void> ()> closing;
    std::function<fw::task_t<void> ()> restoring;

    void add (const event_t &event) { value = value * 10 + event.value; }
    reply_t read (const request_t &) { return {value}; }

    int value = 0;
    std::atomic<unsigned> &restores;

  private:
    fw::instance_spot_context_t _context;
};

class instance_adapter_t final : public fw::spot_relocation_adapter_t<instance_t>
{
  public:
    fw::task_t<std::vector<std::byte>> capture (instance_t &spot, std::stop_token) override
    {
        co_return std::vector<std::byte>{static_cast<std::byte> (spot.value)};
    }

    fw::task_t<void>
    restore (instance_t &spot, std::vector<std::byte> state, std::stop_token) override
    {
        if (spot.restoring)
            co_await spot.restoring ();
        spot.value = std::to_integer<int> (state.at (0));
        ++spot.restores;
        co_return;
    }
};

class InstanceSpotHostRelocation : public ::testing::Test
{
  protected:
    void configure (host_t &host, const char *rid)
    {
        auto &options = host.app.add_zlink_framework ();
        options.add_location_store (locations);
        options.add_relocation_store (relocations);
        options.configure_locations ().owner_lease_renew_interval = 10ms;
        options.configure_locations ().owner_lease_fencing_margin = 1ms;
        options.configure_dispatch ().message_flow (fw::message_flow_log_mode_t::normal);
        host.app.logging ().use_file (std::string (rid) + ".flow");
        options.add_route_mesh (mesh_name)
          .set_object_role (fw::object_role_t::server)
          .set_routing_id (zlink::routing_id_t::from (rid))
          .listen ("tcp://127.0.0.1:0")
          .add_instance_spot_factory<instance_t> (
            stable_type,
            [this, rid] (fw::instance_spot_context_t context) {
                auto spot = std::make_shared<instance_t> (std::move (context), restores);
                if (std::string_view (rid) == source_rid)
                    source_spot = spot;
                else
                    spot->restoring = restoring;
                return spot;
            },
            [] (auto &factory) { factory.template preserve_state_with<instance_adapter_t> (); });
    }

    void start_source ()
    {
        configure (source, source_rid);
        source.start ();
        ASSERT_TRUE (observe ([&] { return source.app.is_ready (); }));
        auto services = source.app.advanced ().services ().build_provider ();
        auto client = source.app.advanced ().zlink ().route_client (
          services.get_required<fw::serializer_registry_t> ());
        const auto activated = client.request_to_spot (spot_id, request_t{})
                                 .instance_spot (stable_type)
                                 .timeout (wait_budget)
                                 .async<reply_t> ()
                                 .result ();
        ASSERT_TRUE (activated) << (activated.error () ? activated.error ()->what () : "");
        ASSERT_EQ (activated.value ().value, 0);
        ASSERT_TRUE (source_spot);
    }

    void start_target ()
    {
        configure (target, target_rid);
        target.start ();
        ASSERT_TRUE (observe ([&] { return target.app.is_ready (); }));
        auto services = source.app.advanced ().services ().build_provider ();
        auto &routes = services.get_required<fw::route_mesh_runtime_t> ();
        ASSERT_TRUE (observe ([&] { return routes.snapshot (mesh_name).ready_peer_count == 1; }));
    }

    fw::authority_read_result_t read_authority ()
    {
        auto services = source.app.advanced ().services ().build_provider ();
        return services.get_required<fw::location_repository_t> ()
          .read_authority (fw::runtime::spot_authority_key (spot_id))
          .result ()
          .value ();
    }

    void relocate ()
    {
        const auto moved = source.app
                             .relocate ({.mode = fw::relocation_mode_t::planned_maintenance,
                                         .deadline = wait_budget})
                             .result ();
        ASSERT_TRUE (moved);
        ASSERT_EQ (moved.value ().outcome, fw::relocation_outcome_t::relocated)
          << "reason=" << static_cast<int> (moved.value ().reason);
    }

    std::atomic<unsigned> restores = 0;
    std::function<fw::task_t<void> ()> restoring;
    std::shared_ptr<instance_t> source_spot;
    std::shared_ptr<fw::runtime::in_memory_location_store_t> locations =
      std::make_shared<fw::runtime::in_memory_location_store_t> ();
    std::shared_ptr<fw::runtime::in_memory_relocation_store_t> relocations =
      std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t source;
    host_t target;
};

TEST_F (InstanceSpotHostRelocation, MovesAnInstanceOnlyHost)
{
    ASSERT_NO_FATAL_FAILURE (start_source ());
    const auto before = std::get<fw::authority_snapshot_t> (read_authority ());
    ASSERT_NO_FATAL_FAILURE (start_target ());
    auto services = target.app.advanced ().services ().build_provider ();
    auto client = target.app.advanced ().zlink ().route_client (
      services.get_required<fw::serializer_registry_t> ());
    const auto read = [&] {
        return client.request_to_spot (spot_id, request_t{})
          .timeout (wait_budget)
          .async<reply_t> ()
          .result ();
    };
    ASSERT_TRUE (client.send_to_spot (spot_id, event_t{7}).async ().result ());
    const auto seeded = read ();
    ASSERT_TRUE (seeded) << (seeded.error () ? seeded.error ()->what () : "");
    ASSERT_EQ (seeded.value ().value, 7);
    auto &resolver = services.get_required<fw::runtime::spot_address_resolver_t> ();
    const auto source_address = resolver.resolve_spot_address (mesh_name, spot_id).result ();
    ASSERT_TRUE (source_address && source_address.value ());
    ASSERT_EQ (source_address.value ()->node_rid.to_string (), source_rid);
    ASSERT_NO_FATAL_FAILURE (relocate ());

    const auto after = std::get<fw::authority_snapshot_t> (read_authority ());
    ASSERT_EQ (after.allocation.target.node_rid.value (), target_rid);
    EXPECT_EQ (after.object_generation, before.object_generation);
    EXPECT_GT (after.authority_owner_generation, before.authority_owner_generation);
    auto application = after;
    fw::runtime::stateful::authority_relocation_payload_codec_t::project_application_payload (
      application);
    const auto ready = fw::runtime::decode_instance_spot_authority_payload (application.payload);
    ASSERT_TRUE (ready);
    EXPECT_EQ (ready->state, fw::runtime::instance_spot_authority_state_t::ready);
    EXPECT_EQ (ready->spot_id, spot_id);
    EXPECT_EQ (ready->stable_type, stable_type);
    EXPECT_EQ (ready->owner_id, after.owner.owner_id);
    EXPECT_EQ (ready->owner_lease_generation, after.owner.lease_generation);
    EXPECT_EQ (ready->node_rid.value (), target_rid);
    EXPECT_EQ (ready->node_generation, after.allocation.target.node_lifecycle_generation);
    EXPECT_EQ (restores.load (), 1u);

    // The submitted messages keep their pre-relocation address even if the
    // live resolver has already received the Store commit.
    auto channels =
      fw::detail::channel_runtime_t::from (target.app.advanced ().zlink ().message_bus ());
    channels.bind_spot_address_resolver (
      std::make_shared<retained_address_resolver_t> (*source_address.value ()));
    ASSERT_TRUE (client.send_to_spot (spot_id, event_t{8}).async ().result ());
    const auto received = read ();
    ASSERT_TRUE (received) << (received.error () ? received.error ()->what () : "");
    EXPECT_EQ (received.value ().value, 78);
    const auto late_close = source_spot->context ().close ().result ();
    ASSERT_FALSE (late_close);
    EXPECT_EQ (late_close.error_kind (), fw::framework_error_kind_t::unavailable);
    EXPECT_EQ (
      fw::detail::failure_code (*late_close.error ()),
      static_cast<std::uint32_t> (fw::runtime::protocol::framework_error_code::spotMoving));
    const auto after_close = std::get<fw::authority_snapshot_t> (read_authority ());
    EXPECT_TRUE (after_close.same_incarnation (after));
}

TEST_F (InstanceSpotHostRelocation, RestoresHeldDirectMessagesAndLogicalTimerAfterCommit)
{
    auto restoring_started = std::make_shared<fw::task_completion_source_t<void>> ();
    auto continue_restore = std::make_shared<fw::task_completion_source_t<void>> ();
    restoring = [restoring_started, continue_restore] () -> fw::task_t<void> {
        restoring_started->complete (fw::result_t<void>::success ());
        co_await continue_restore->task ();
    };
    const auto release = std::shared_ptr<void> (nullptr, [continue_restore] (void *) {
        if (!continue_restore->task ().await_ready ())
            continue_restore->complete (fw::result_t<void>::success ());
    });
    ASSERT_NO_FATAL_FAILURE (start_source ());
    ASSERT_NO_FATAL_FAILURE (start_target ());

    const auto source_runtime =
      fw::detail::spot_node_runtime_t::from (source.app.advanced ().zlink (), mesh_name);
    const auto target_runtime =
      fw::detail::spot_node_runtime_t::from (target.app.advanced ().zlink (), mesh_name);
    ASSERT_TRUE (source_runtime);
    ASSERT_TRUE (target_runtime);
    auto source_native = source_runtime->native_node ();
    auto target_native = target_runtime->native_node ();
    const auto owner =
      source_native->objects ().find (stateful::object_kind_t::instance_spot, spot_id);
    ASSERT_TRUE (owner);
    constexpr std::uint64_t next_due = 2000000000000ULL;
    const stateful::logical_timer_t timer{
      .timer_id = 1,
      .due_after_milliseconds = next_due,
      .period_milliseconds = 1000,
      .next_tick_sequence = 2,
      .name = "pulse",
      .handler_type = "test.pulse",
      .overrun_policy = 2,
      .max_catch_up_ticks = 2,
      .last_completed_delivery_index = 1,
      .last_completed_scheduled_index = 1,
      .next_scheduled_at_unix_milliseconds = next_due,
      .pending_ticks = {{2, 2, next_due - 2000, 0}, {3, 3, next_due - 1000, 0}}};
    ASSERT_EQ (source_native->objects ().register_timer (*owner, timer),
               stateful::stateful_error_t::none);

    auto services = target.app.advanced ().services ().build_provider ();
    auto client = target.app.advanced ().zlink ().route_client (
      services.get_required<fw::serializer_registry_t> ());
    ASSERT_TRUE (client.request_to_spot (spot_id, request_t{})
                   .timeout (wait_budget)
                   .async<reply_t> ()
                   .result ());
    const auto moving = source.app.relocate (
      {.mode = fw::relocation_mode_t::planned_maintenance, .deadline = wait_budget});
    ASSERT_TRUE (observe ([&] { return restoring_started->task ().await_ready (); }));
    const auto precommit = std::get<fw::authority_snapshot_t> (read_authority ());
    EXPECT_EQ (precommit.allocation.target.node_rid.value (), source_rid);
    EXPECT_FALSE (target_native->objects ().find (stateful::object_kind_t::instance_spot, spot_id,
                                                  stateful::object_state_t::ready));
    for (int value : {1, 2})
        ASSERT_TRUE (client.send_to_spot (spot_id, event_t{value}).async ().result ());
    ASSERT_TRUE (observe ([&] {
        return source_native->objects ().pending (*owner, stateful::turn_domain_t::application)
               == 2;
    }));

    continue_restore->complete (fw::result_t<void>::success ());
    const auto moved = moving.result ();
    ASSERT_TRUE (moved);
    ASSERT_EQ (moved.value ().outcome, fw::relocation_outcome_t::relocated)
      << "reason=" << static_cast<int> (moved.value ().reason);
    const auto replayed = client.request_to_spot (spot_id, request_t{})
                            .timeout (wait_budget)
                            .async<reply_t> ()
                            .result ();
    ASSERT_TRUE (replayed) << (replayed.error () ? replayed.error ()->what () : "");
    EXPECT_EQ (replayed.value ().value, 12);
    EXPECT_EQ (restores.load (), 1u);
    const auto relocated =
      target_native->objects ().find (stateful::object_kind_t::instance_spot, spot_id);
    ASSERT_TRUE (relocated);
    EXPECT_EQ (target_native->objects ().timers (*relocated),
               std::vector<stateful::logical_timer_t>{timer});
}

TEST_F (InstanceSpotHostRelocation, SkipsAnInstanceWhoseClosingFenceCommittedFirst)
{
    ASSERT_NO_FATAL_FAILURE (start_source ());
    auto closing_started = std::make_shared<fw::task_completion_source_t<void>> ();
    auto continue_close = std::make_shared<fw::task_completion_source_t<void>> ();
    source_spot->closing = [closing_started, continue_close] () -> fw::task_t<void> {
        closing_started->complete (fw::result_t<void>::success ());
        co_await continue_close->task ();
    };
    const auto release = std::shared_ptr<void> (nullptr, [continue_close] (void *) {
        if (!continue_close->task ().await_ready ())
            continue_close->complete (fw::result_t<void>::success ());
    });
    const auto closing = source_spot->context ().close ();
    ASSERT_TRUE (observe ([&] { return closing_started->task ().await_ready (); }));
    const auto current = std::get<fw::authority_snapshot_t> (read_authority ());
    ASSERT_TRUE (fw::runtime::decode_instance_closing_state (current.payload));

    ASSERT_NO_FATAL_FAILURE (relocate ());
    const auto during_close = std::get<fw::authority_snapshot_t> (read_authority ());
    EXPECT_EQ (during_close.store_version, current.store_version);
    EXPECT_EQ (restores.load (), 0u);

    continue_close->complete (fw::result_t<void>::success ());
    const auto closed = closing.result ();
    ASSERT_TRUE (closed);
    ASSERT_TRUE (closed.value ());
    EXPECT_TRUE (std::holds_alternative<fw::authority_missing_t> (read_authority ()));
}
}

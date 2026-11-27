/* SPDX-License-Identifier: Elastic-2.0 */
#include <zlink/framework.hpp>
#include <zlink/json.hpp>

#include "host_lifecycle_ready_probe.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "telemetry_observation.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <tuple>

namespace
{
namespace fw = zlink::framework;
using namespace std::chrono_literals;
constexpr auto wait_budget = 5s;

struct snapshot_t
{
    static constexpr auto packet_name = "dispatch-failure.snapshot";
    double winner_score{};
};

struct spot_message_t
{
    static constexpr auto packet_name = "dispatch-failure.spot";
    bool fail{};
    std::int64_t sequence{};
};
}

template <> struct zlink::json::dto_traits<snapshot_t>
{
    static constexpr auto fields =
      std::tuple{zlink::json::field ("winnerScore", &snapshot_t::winner_score)};
};

template <> struct zlink::json::dto_traits<spot_message_t>
{
    static constexpr auto fields =
      std::tuple{zlink::json::field ("fail", &spot_message_t::fail),
                 zlink::json::field ("sequence", &spot_message_t::sequence)};
};

namespace
{
struct handler_observations_t
{
    std::promise<double> score;
    std::promise<void> spot_worker_entered;
    std::promise<void> release_spot_worker;
};

class actor_t final : public fw::actor_t
{
  public:
    explicit actor_t (fw::actor_context_t context) : _context (std::move (context)) {}
    fw::actor_context_t &context () noexcept override { return _context; }
    const fw::actor_context_t &context () const noexcept override { return _context; }

  private:
    fw::actor_context_t _context;
};

class actor_factory_t final : public fw::actor_factory_t<actor_t>
{
  public:
    fw::task_t<std::shared_ptr<actor_t>> create (fw::actor_context_t context,
                                                 std::stop_token) override
    {
        co_return std::make_shared<actor_t> (std::move (context));
    }
};

class entry_spot_t final : public fw::entry_spot_t<actor_t>
{
  public:
    entry_spot_t (fw::entry_spot_context_t context,
                  std::shared_ptr<handler_observations_t> observations) :
        _context (std::move (context)), _observations (std::move (observations))
    {
    }

    fw::entry_spot_context_t &context () noexcept override { return _context; }
    const fw::entry_spot_context_t &context () const noexcept override { return _context; }
    void configure () override
    {
        _context.handlers ().add_actor_send<&entry_spot_t::on_snapshot> ();
    }
    fw::task_t<void> on_actor_joined (actor_t &) override { co_return; }
    fw::task_t<void> on_leave_actor (actor_t &) override { co_return; }

    void on_snapshot (actor_t &, fw::message_context_t &, const snapshot_t &snapshot)
    {
        _observations->score.set_value (snapshot.winner_score);
    }

  private:
    fw::entry_spot_context_t _context;
    std::shared_ptr<handler_observations_t> _observations;
};

class user_spot_t final : public fw::spot_t<actor_t>
{
  public:
    user_spot_t (fw::spot_context_t context, std::shared_ptr<handler_observations_t> observations) :
        _context (std::move (context)), _observations (std::move (observations))
    {
    }

    fw::spot_context_t &context () noexcept override { return _context; }
    const fw::spot_context_t &context () const noexcept override { return _context; }
    void configure () override { _context.handlers ().add_handler<&user_spot_t::on_message> (); }
    fw::task_t<fw::spot_actor_join_result_t> on_actor_join (std::string_view,
                                                            const fw::message_t &) override
    {
        co_return fw::spot_actor_join_result_t::reject ();
    }
    fw::task_t<void> on_actor_joined (actor_t &) override { co_return; }
    fw::task_t<void> on_leave_actor (actor_t &) override { co_return; }

    fw::task_t<void> on_message (const spot_message_t &message)
    {
        if (!message.fail)
            co_return;
        auto worker = _context.run_cpu_worker ([observations = _observations] {
            auto release = observations->release_spot_worker.get_future ();
            observations->spot_worker_entered.set_value ();
            if (release.wait_for (wait_budget) != std::future_status::ready)
                throw std::runtime_error ("test worker release timed out");
        });
        co_await worker.async ();
        throw std::runtime_error ("Spot handler failed after awaiting worker");
    }

  private:
    fw::spot_context_t _context;
    std::shared_ptr<handler_observations_t> _observations;
};

std::string_view field (const fw::log_record_t &event, std::string_view key)
{
    const auto found = std::ranges::find (event.fields, key, &fw::log_field_t::key);
    return found == event.fields.end () ? std::string_view{} : std::string_view (found->value);
}

std::vector<fw::log_record_t> matching (const std::vector<fw::log_record_t> &events,
                                        std::string_view packet,
                                        std::string_view key,
                                        std::string_view value)
{
    std::vector<fw::log_record_t> matches;
    for (const auto &event : events)
        if (field (event, "packet") == packet && field (event, key) == value)
            matches.push_back (event);
    return matches;
}

class DispatchFailureFlow : public testing::Test
{
  protected:
    void SetUp () override
    {
        app.logging ().use_console ().use_provider ("dispatch-failure-test",
                                                    [records = records] (const auto &event) {
                                                        if (!field (event, "packet").empty ())
                                                            records->record (event);
                                                    });
        auto &options = app.add_zlink_framework ();
        options.configure_dispatch ().message_flow (fw::message_flow_log_mode_t::normal);
        options.add_location_store (std::make_shared<fw::runtime::in_memory_location_store_t> ());
        options.add_relocation_store (
          std::make_shared<fw::runtime::in_memory_relocation_store_t> ());
        options.add_route_mesh ("dispatch-failure")
          .set_object_role (fw::object_role_t::server)
          .set_routing_id (zlink::routing_id_t::from ("dispatch-failure"))
          .listen ("tcp://127.0.0.1:0")
          .add_entry_spot<entry_spot_t> ([observations = observations] (auto context) {
              return std::make_shared<entry_spot_t> (std::move (context), observations);
          })
          .add_actor_factory<actor_t, actor_factory_t> (
            "player", std::make_shared<actor_factory_t> (),
            [] (auto &factory) { factory.recreate_on_relocation (); })
          .add_spot_factory<user_spot_t> (
            "room",
            [observations = observations] (auto context) {
                return std::make_shared<user_spot_t> (std::move (context), observations);
            },
            [] (auto &factory) { factory.disable_relocation (); });
        auto ready = std::make_unique<fw::test::host_lifecycle::ready_host_probe_t> ();
        auto *probe = ready.get ();
        app.add_hosted_service (std::move (ready));
        host = std::thread ([this] {
            char name[] = "dispatch-failure";
            char *argv[]{name, nullptr};
            app.run (1, argv);
        });
        ASSERT_TRUE (probe->wait_ready ());
    }

    void TearDown () override
    {
        if (host.joinable ()) {
            auto stopped = app.shutdown (1s);
            host.join ();
            EXPECT_TRUE (stopped.result ());
        }
    }

    bool wait_for_event (std::string_view packet, std::string_view key, std::string_view value)
    {
        std::unique_lock lock (records->mutex);
        return records->changed.wait_for (lock, wait_budget, [&] {
            return std::ranges::any_of (records->events, [&] (const auto &event) {
                return field (event, "packet") == packet && field (event, key) == value;
            });
        });
    }

    void drain_records ()
    {
        fw::test::acknowledge_delivery (std::chrono::steady_clock::now () + wait_budget);
    }

    std::shared_ptr<handler_observations_t> observations =
      std::make_shared<handler_observations_t> ();
    std::shared_ptr<fw::test::telemetry_observations_t<fw::log_record_t>> records =
      std::make_shared<fw::test::telemetry_observations_t<fw::log_record_t>> ();
    fw::app_t app = fw::app_t::create ();
    std::thread host;
};

TEST_F (DispatchFailureFlow, ActorEncodeFailureIsReportedBeforeSubmission)
{
    auto services = app.advanced ().services ().build_provider ();
    auto &actors = services.get_required<fw::actor_manager_t> ();
    auto &client = services.get_required<fw::actor_client_t> ();
    const fw::actor_id_t id ("player-1");
    ASSERT_TRUE (actors.get_or_create (id, "player").async ().result ());

    testing::internal::CaptureStderr ();
    const auto rejected =
      client.send (id, snapshot_t{std::numeric_limits<double>::infinity ()}).async ().result ();
    drain_records ();
    std::clog.flush ();
    const auto stderr_output = testing::internal::GetCapturedStderr ();
    ASSERT_FALSE (rejected);
    ASSERT_NE (rejected.error (), nullptr);
    EXPECT_EQ (rejected.error_kind (), fw::framework_error_kind_t::protocol_error);
    const std::string_view error = rejected.error ()->what ();
    EXPECT_NE (error.find (snapshot_t::packet_name), std::string_view::npos);
    EXPECT_NE (error.find ("winnerScore"), std::string_view::npos);
    EXPECT_NE (error.find ("finite JSON number"), std::string_view::npos);
    EXPECT_NE (stderr_output.find ("event_id=zlink.dispatch_error"), std::string::npos);
    EXPECT_NE (stderr_output.find ("reason=encode_error"), std::string::npos);
    EXPECT_NE (stderr_output.find ("outcome=failed"), std::string::npos);
    EXPECT_NE (stderr_output.find ("action=fail_caller"), std::string::npos);
    EXPECT_NE (stderr_output.find ("winnerScore"), std::string::npos);
    const auto rejected_events = records->snapshot ();
    const auto failures =
      matching (rejected_events, snapshot_t::packet_name, "event_id", "zlink.dispatch_error");
    EXPECT_TRUE (matching (rejected_events, snapshot_t::packet_name, "phase", "sent").empty ());
    EXPECT_EQ (failures.size (), 1u);
    if (!failures.empty ()) {
        const auto &event = failures.front ();
        EXPECT_EQ (field (event, "outcome"), "failed");
        EXPECT_EQ (field (event, "reason"), "encode_error");
        EXPECT_EQ (field (event, "action"), "fail_caller");
        EXPECT_NE (field (event, "error_message").find ("winnerScore"), std::string_view::npos);
        EXPECT_NE (field (event, "error_message").find ("non-finite"), std::string_view::npos);
    }

    records->clear ();
    auto score = observations->score.get_future ();
    ASSERT_TRUE (client.send (id, snapshot_t{12.5}).async ().result ());
    ASSERT_EQ (score.wait_for (wait_budget), std::future_status::ready);
    EXPECT_EQ (score.get (), 12.5);
    drain_records ();
    EXPECT_TRUE (
      matching (records->snapshot (), snapshot_t::packet_name, "event_id", "zlink.dispatch_error")
        .empty ());
}

TEST_F (DispatchFailureFlow, AsyncSpotHandlerFailureRetainsReceivedFlow)
{
    auto services = app.advanced ().services ().build_provider ();
    auto &spots = services.get_required<fw::spot_manager_t> ();
    auto &route = services.get_required<fw::route_client_t> ();
    ASSERT_TRUE (spots.get_or_create ("room-1", "room").async ().result ());

    auto worker_entered = observations->spot_worker_entered.get_future ();
    testing::internal::CaptureStderr ();
    auto sending = route.send_to_spot ("room-1", spot_message_t{true}).async ();
    const auto entered = worker_entered.wait_for (wait_budget);
    observations->release_spot_worker.set_value ();
    const auto result = sending.result ();
    const auto failed = wait_for_event (spot_message_t::packet_name, "reason", "handler_exception");
    drain_records ();
    std::clog.flush ();
    const auto stderr_output = testing::internal::GetCapturedStderr ();
    ASSERT_TRUE (result);
    ASSERT_EQ (entered, std::future_status::ready);
    EXPECT_TRUE (failed);
    EXPECT_NE (stderr_output.find ("event_id=zlink.dispatch_error"), std::string::npos);
    EXPECT_NE (stderr_output.find ("reason=handler_exception"), std::string::npos);
    EXPECT_NE (stderr_output.find ("outcome=failed"), std::string::npos);
    EXPECT_NE (stderr_output.find ("action=drop"), std::string::npos);
    EXPECT_NE (stderr_output.find ("Spot handler failed after awaiting worker"), std::string::npos);

    const auto rejected_events = records->snapshot ();
    const auto received =
      matching (rejected_events, spot_message_t::packet_name, "phase", "received");
    const auto failures =
      matching (rejected_events, spot_message_t::packet_name, "event_id", "zlink.dispatch_error");
    EXPECT_EQ (received.size (), 1u);
    EXPECT_EQ (failures.size (), 1u);
    EXPECT_TRUE (
      matching (rejected_events, spot_message_t::packet_name, "phase", "completed").empty ());
    if (!received.empty () && !failures.empty ()) {
        const auto &event = failures.front ();
        EXPECT_FALSE (field (received.front (), "flow").empty ());
        EXPECT_EQ (field (event, "flow"), field (received.front (), "flow"));
        EXPECT_FALSE (field (received.front (), "origin").empty ());
        EXPECT_EQ (field (event, "origin"), field (received.front (), "origin"));
        EXPECT_EQ (field (event, "outcome"), "failed");
        EXPECT_EQ (field (event, "reason"), "handler_exception");
        EXPECT_EQ (field (event, "action"), "drop");
        EXPECT_EQ (field (event, "error_message"), "Spot handler failed after awaiting worker");
    }

    records->clear ();
    ASSERT_TRUE (route.send_to_spot ("room-1", spot_message_t{false}).async ().result ());
    EXPECT_TRUE (wait_for_event (spot_message_t::packet_name, "phase", "completed"));
    drain_records ();
    EXPECT_TRUE (matching (records->snapshot (), spot_message_t::packet_name, "event_id",
                           "zlink.dispatch_error")
                   .empty ());
}

TEST (DispatchFailureCodec, DecodeExplainsNullAndIntegerViolations)
{
    const auto serializer = fw::serializer_registry_t ().get<spot_message_t> ();
    for (auto [wire, field_name, advice] :
         {std::tuple{R"({"fail":null,"sequence":"1"})", "fail", "std::optional"},
          std::tuple{R"({"fail":false,"sequence":1})", "sequence", "64-bit decimal string"}}) {
        try {
            (void) serializer.deserialize (fw::encoded_payload_t::from_string (wire));
            ADD_FAILURE () << "Invalid wire value was accepted for " << field_name;
        }
        catch (const fw::framework_exception_t &error) {
            EXPECT_EQ (error.kind (), fw::framework_error_kind_t::protocol_error);
            const std::string_view message = error.what ();
            EXPECT_NE (message.find (spot_message_t::packet_name), std::string_view::npos);
            EXPECT_NE (message.find (std::string (field_name) + ": "), std::string_view::npos);
            EXPECT_NE (message.find (advice), std::string_view::npos);
        }
    }
}
}

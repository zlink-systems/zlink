/* SPDX-License-Identifier: Elastic-2.0 */
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/diagnostics/flow_context.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/messaging/request_deadline.hpp"
#include "runtime/timers/timer_runtime.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "../support/host_readiness.hpp"
#include "../support/telemetry_observation.hpp"
#include "../support/manual_internal_scheduler.hpp"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <zlink/json.hpp>
#include <zlink/framework.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "../support/spot_close_preparation_fixture.hpp"
#include "../support/spot_close_actor_fixture.hpp"

namespace
{
using namespace zlink::framework::test::spot_close;
using namespace std::chrono_literals;
namespace zf = zlink::framework;

using zlink::framework::test::wait_for_serving;

const auto public_callback_marker = std::make_shared<const int> (369306);
const auto public_callback_original =
  std::make_exception_ptr (empty_callback_failure_t (public_callback_marker));

class callback_cause_spot_t final : public zf::spot_t<member_actor_t>
{
  public:
    explicit callback_cause_spot_t (zf::spot_context_t context) : _context (std::move (context)) {}
    zf::spot_context_t &context () noexcept override { return _context; }
    const zf::spot_context_t &context () const noexcept override { return _context; }
    void configure () override {}
    zf::task_t<zf::spot_create_response_t> on_create (const zf::message_t &) override
    {
        std::rethrow_exception (public_callback_original);
        co_return zf::spot_create_response_t::accept ();
    }
    zf::task_t<zf::spot_actor_join_result_t> on_actor_join (std::string_view,
                                                            const zf::message_t &) override
    {
        co_return zf::spot_actor_join_result_t::accept ();
    }
    zf::task_t<void> on_actor_joined (member_actor_t &) override { co_return; }
    zf::task_t<void> on_leave_actor (member_actor_t &) override { co_return; }

  private:
    zf::spot_context_t _context;
};

class callback_cause_entry_t final : public zf::entry_spot_t<member_actor_t>
{
  public:
    explicit callback_cause_entry_t (zf::entry_spot_context_t context) :
        _context (std::move (context))
    {
    }
    zf::entry_spot_context_t &context () noexcept override { return _context; }
    const zf::entry_spot_context_t &context () const noexcept override { return _context; }
    void configure () override {}
    zf::task_t<zf::actor_create_response_t> on_create_actor (member_actor_t &,
                                                             const zf::message_t &) override
    {
        std::rethrow_exception (public_callback_original);
        co_return zf::actor_create_response_t::accept ();
    }
    zf::task_t<void> on_actor_joined (member_actor_t &) override { co_return; }
    zf::task_t<void> on_leave_actor (member_actor_t &) override { co_return; }

  private:
    zf::entry_spot_context_t _context;
};

class callback_cause_factory_t final : public zf::actor_factory_t<member_actor_t>
{
  public:
    explicit callback_cause_factory_t (bool fail) : _fail (fail) {}
    zf::task_t<std::shared_ptr<member_actor_t>> create (zf::actor_context_t context,
                                                        std::stop_token) override
    {
        if (_fail)
            std::rethrow_exception (public_callback_original);
        co_return std::make_shared<member_actor_t> (std::move (context));
    }

  private:
    bool _fail;
};

class callback_cause_client_t final : public zf::hosted_service_t
{
  public:
    explicit callback_cause_client_t (zf::app_t &app, bool actor = false) :
        _app (app), _actor (actor)
    {
    }
    void stop () noexcept override {}
    zf::task_t<void> start (zf::service_provider_t &services) override
    {
        _worker = std::jthread ([this, services] () mutable {
            wait_for_serving (services);
            if (_actor) {
                verify (services.get_required<zf::actor_manager_t> ()
                          .get_or_create (zf::actor_id_t ("callback-cause"), "failing")
                          .async ()
                          .result ());
            } else {
                verify (services.get_required<zf::spot_manager_t> ()
                          .get_or_create (zf::spot_id_t ("callback-cause"), "failing")
                          .async ()
                          .result ());
            }
            _app.stop ();
        });
        co_return;
    }

  private:
    template <typename T> void verify (const zf::result_t<T> &result)
    {
        EXPECT_FALSE (result);
        try {
            result.value ();
            ADD_FAILURE () << "callback must fail";
        }
        catch (const empty_callback_failure_t &error) {
            EXPECT_EQ (public_callback_marker, error.marker);
            EXPECT_STREQ ("", error.what ());
        }
        catch (const std::exception &error) {
            ADD_FAILURE () << "original callback exception lost: " << error.what ();
        }
    }
    zf::app_t &_app;
    bool _actor;
    std::jthread _worker;
};

TEST (LocalCallbackCause, PublicUserSpotOnCreatePreservesEmptyApplicationException)
{
    auto app = zf::app_t::create ();
    app.logging ().use_file ("callback-cause.flow").set_min_level (zf::log_level_t::debug);
    app.add_zlink_framework ([] (zf::zlink_framework_options_t &options) {
        options.configure_dispatch ().message_flow (zf::message_flow_log_mode_t::detailed);
        options.configure_locations ().owner_lease_renew_interval = std::chrono::milliseconds (10);
        options.configure_locations ().owner_lease_fencing_margin = std::chrono::milliseconds (1);
        options.add_location_store (std::make_shared<zf::runtime::in_memory_location_store_t> ());
        options.add_route_mesh ("callback-cause")
          .set_object_role (zf::object_role_t::server)
          .set_routing_id (zlink::routing_id_t::from ("callback-cause-node"))
          .listen ("tcp://127.0.0.1:0")
          .add_spot_factory<callback_cause_spot_t> (
            "failing",
            [] (zf::spot_context_t context) {
                return std::make_shared<callback_cause_spot_t> (std::move (context));
            },
            [] (auto &factory) { factory.disable_relocation (); });
    });
    app.add_hosted_service (std::make_unique<callback_cause_client_t> (app));
    EXPECT_EQ (0, app.run (0, nullptr));
}

void verify_actor_callback_cause (bool factory_failure)
{
    auto app = zf::app_t::create ();
    app.add_zlink_framework ([factory_failure] (zf::zlink_framework_options_t &options) {
        options.configure_locations ().owner_lease_renew_interval = std::chrono::milliseconds (10);
        options.configure_locations ().owner_lease_fencing_margin = std::chrono::milliseconds (1);
        options.add_location_store (std::make_shared<zf::runtime::in_memory_location_store_t> ());
        options.add_route_mesh ("callback-cause")
          .set_object_role (zf::object_role_t::server)
          .set_routing_id (zlink::routing_id_t::from ("callback-cause-node"))
          .listen ("tcp://127.0.0.1:0")
          .add_entry_spot<callback_cause_entry_t> ([] (zf::entry_spot_context_t context) {
              return std::make_shared<callback_cause_entry_t> (std::move (context));
          })
          .add_actor_factory<member_actor_t, callback_cause_factory_t> (
            "failing", std::make_shared<callback_cause_factory_t> (factory_failure),
            [] (auto &factory) { factory.disable_relocation (); });
    });
    app.add_hosted_service (std::make_unique<callback_cause_client_t> (app, true));
    EXPECT_EQ (0, app.run (0, nullptr));
}

TEST (LocalCallbackCause, ActorFactoryPreservesEmptyApplicationException)
{
    verify_actor_callback_cause (true);
}

TEST (LocalCallbackCause, ActorJoinPreservesEmptyApplicationException)
{
    verify_actor_callback_cause (false);
}
} // namespace

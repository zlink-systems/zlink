/* SPDX-License-Identifier: Elastic-2.0 */
#include "../support/host_readiness.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/utils/available_processor_count.hpp"
#include <zlink/framework.hpp>
#include <zlink/json.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>

namespace fw = zlink::framework;
using namespace std::chrono_literals;

struct suspended_outer_t
{
    static constexpr const char *packet_name = "SuspendedOuter";
    int id{};
};
struct suspended_inner_t
{
    static constexpr const char *packet_name = "SuspendedInner";
    int id{};
};
struct suspended_reply_t
{
    int id{};
    bool ok{};
};
template <> struct zlink::json::dto_traits<suspended_outer_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field ("id", &suspended_outer_t::id)};
};
template <> struct zlink::json::dto_traits<suspended_inner_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field ("id", &suspended_inner_t::id)};
};
template <> struct zlink::json::dto_traits<suspended_reply_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field ("id", &suspended_reply_t::id),
                                              zlink::json::field ("ok", &suspended_reply_t::ok)};
};

namespace
{
struct probe_t
{
    std::atomic<int> arrived{};
    fw::task_completion_source_t<void> both;
};

class room_t final : public fw::spot_t<fw::actor_t>
{
    fw::spot_context_t _context;
    std::shared_ptr<probe_t> _probe;

  public:
    room_t (fw::spot_context_t context, std::shared_ptr<probe_t> probe) :
        _context (std::move (context)), _probe (std::move (probe))
    {
    }
    fw::spot_context_t &context () noexcept override { return _context; }
    const fw::spot_context_t &context () const noexcept override { return _context; }
    void configure () override
    {
        _context.handlers ().add_handler<&room_t::outer> ("SuspendedOuter");
        _context.handlers ().add_handler<&room_t::inner> ("SuspendedInner");
    }
    fw::task_t<fw::spot_actor_join_result_t> on_actor_join (std::string_view,
                                                            const fw::message_t &) override
    {
        co_return fw::spot_actor_join_result_t::reject ();
    }
    fw::task_t<void> on_actor_joined (fw::actor_t &) override { co_return; }
    fw::task_t<void> on_leave_actor (fw::actor_t &) override { co_return; }
    fw::task_t<suspended_reply_t> outer (const suspended_outer_t &request)
    {
        if (++_probe->arrived == 2)
            _probe->both.complete (fw::result_t<void>::success ());
        co_await _probe->both.task ();
        co_return co_await _context
          .request_to_spot (fw::spot_id_t ("b"), suspended_inner_t{request.id})
          .timeout (2s)
          .async<suspended_reply_t> ();
    }
    suspended_reply_t inner (const suspended_inner_t &request) { return {request.id, true}; }
};

class client_t final : public fw::hosted_service_t
{
    fw::app_t &_app;
    std::jthread _worker;

  public:
    explicit client_t (fw::app_t &app) : _app (app) {}
    void stop () noexcept override {}
    fw::task_t<void> start (fw::service_provider_t &services) override
    {
        _worker = std::jthread ([this, services] () mutable {
            try {
                fw::test::wait_for_serving (services);
                auto &manager = services.get_required<fw::spot_manager_t> ();
                for (const auto *id : {"b", "a0", "a1"})
                    manager.get_or_create (fw::spot_id_t (id), "room")
                      .in_mesh ("suspended-workers")
                      .async ()
                      .result ()
                      .value ();
                auto route = _app.advanced ().zlink ().route_client (
                  services.get_required<fw::serializer_registry_t> ());
                auto first = route.request_to_spot (fw::spot_id_t ("a0"), suspended_outer_t{0})
                               .timeout (2s)
                               .async<suspended_reply_t> ();
                auto second = route.request_to_spot (fw::spot_id_t ("a1"), suspended_outer_t{1})
                                .timeout (2s)
                                .async<suspended_reply_t> ();
                const auto a = first.result ().value ();
                const auto b = second.result ().value ();
                EXPECT_EQ (0, a.id);
                EXPECT_TRUE (a.ok);
                EXPECT_EQ (1, b.id);
                EXPECT_TRUE (b.ok);
            }
            catch (const std::exception &error) {
                ADD_FAILURE () << error.what ();
            }
            _app.stop ();
        });
        co_return;
    }
};

int run_scenario ()
{
    auto probe = std::make_shared<probe_t> ();
    auto app = fw::app_t::create ();
    app.add_zlink_framework ([&] (fw::zlink_framework_options_t &options) {
        options.add_location_store (std::make_shared<fw::runtime::in_memory_location_store_t> ());
        options.add_route_mesh ("suspended-workers")
          .listen ("tcp://127.0.0.1:0")
          .set_automatic_routing_id_prefix ("suspended-workers")
          .set_object_role (fw::object_role_t::server)
          .add_spot_factory<room_t> (
            "room",
            [probe] (fw::spot_context_t context) {
                return std::make_shared<room_t> (std::move (context), probe);
            },
            [] (auto &factory) { factory.disable_relocation (); });
    });
    app.add_hosted_service (std::make_unique<client_t> (app));
    EXPECT_EQ (0, app.run (0, nullptr));
    EXPECT_EQ (2, probe->arrived.load ());
    return ::testing::Test::HasFailure () ? 1 : 0;
}
} // namespace

TEST (SuspendedHandlerWorkerDeathTest, TwoSpotHandlersReturnWorkersForAnotherSpot)
{
#ifdef __linux__
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    EXPECT_EXIT (
      {
          cpu_set_t available;
          cpu_set_t limited;
          CPU_ZERO (&available);
          CPU_ZERO (&limited);
          if (sched_getaffinity (getpid (), sizeof (available), &available) != 0)
              std::_Exit (2);
          unsigned selected = 0;
          for (int cpu = 0; cpu < CPU_SETSIZE && selected < 2; ++cpu) {
              if (CPU_ISSET (cpu, &available)) {
                  CPU_SET (cpu, &limited);
                  ++selected;
              }
          }
          if (sched_setaffinity (getpid (), sizeof (limited), &limited) != 0)
              std::_Exit (2);
          if (std::max (2u, fw::runtime::available_processor_count ()) != 2)
              std::_Exit (2);
          std::_Exit (run_scenario ());
      },
      ::testing::ExitedWithCode (0), "");
#else
    GTEST_SKIP () << "Two-worker pool sizing uses Linux process affinity";
#endif
}

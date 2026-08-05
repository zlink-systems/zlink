/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/spots/spot_runtime.hpp"

#include <zlink/framework.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{

constexpr std::size_t actor_count = 200;
constexpr std::size_t packets_per_actor = 50;
constexpr std::size_t warmup_packets = 2000;
constexpr std::size_t rounds = 5;

class player_actor_t final
{
  public:
    player_actor_t () = default;
    explicit player_actor_t (std::size_t value) : index (value) {}

    std::size_t index{};
};

struct admission_request_t
{
    int packet{};
};

struct admission_entry_spot_t
{
    using actor_type = player_actor_t;

    void configure (zlink::framework::spot_context_t &context)
    {
        context.handlers ().add_actor_send<&admission_entry_spot_t::on_admission> (
          "admission.move");
    }

    void on_admission (player_actor_t &actor,
                       const zlink::framework::message_context_t &context,
                       const admission_request_t &request)
    {
        admitted.fetch_add (1, std::memory_order_relaxed);
        std::lock_guard lock (mutex);
        pending_actor = actor.index;
        pending_packet = request.packet;
        pending_packet_name = context.packet_name;
    }

    zlink::framework::task_t<zlink::framework::spot_actor_join_result_t>
    on_actor_join (std::string_view, const zlink::framework::message_t &)
    {
        co_return zlink::framework::spot_actor_join_result_t::accept ();
    }

    zlink::framework::task_t<void> on_actor_joined (player_actor_t &)
    {
        co_return;
    }

    zlink::framework::task_t<void> on_leave_actor (player_actor_t &)
    {
        co_return;
    }

    zlink::framework::task_t<void> on_disconnect_actor (player_actor_t &)
    {
        co_return;
    }

    std::atomic<std::size_t> admitted{0};
    std::mutex mutex;
    std::size_t pending_actor{};
    int pending_packet{};
    std::string pending_packet_name;
};

struct benchmark_result_t
{
    double wall_ms{};
    double throughput{};
    double p50_us{};
    double p95_us{};
    double p99_us{};
    double max_us{};
};

struct fixture_t
{
    std::shared_ptr<zlink::framework::detail::spot_context_state_t> state;
    zlink::framework::spot_context_t context;
    admission_entry_spot_t spot;
    zlink::framework::service_provider_t services;
    zlink::framework::serializer_registry_t serializers;
    std::vector<player_actor_t> actors;

    explicit fixture_t (bool serial) :
        state (std::make_shared<zlink::framework::detail::spot_context_state_t> ()),
        context (zlink::framework::detail::spot_context_access_t::create (state)),
        services (zlink::framework::service_collection_t ().build_provider ())
    {
        if (serial) {
            state->serial_executor =
              std::make_shared<zlink::framework::runtime::offload_executor_t> (1);
            state->serial_queue =
              std::make_shared<zlink::framework::runtime::serial_execution_queue_t> (
                *state->serial_executor);
        }

        serializers.add<admission_request_t> (
          [] (const admission_request_t &value) {
              return zlink::framework::encoded_payload_t::from_string (
                std::to_string (value.packet));
          },
          [] (const zlink::framework::encoded_payload_t &payload) {
              return admission_request_t{std::stoi (payload.to_string ())};
          });
        actors.reserve (actor_count);
        for (std::size_t index = 0; index < actor_count; ++index) {
            actors.push_back (player_actor_t{index});
        }
        spot.configure (context);
    }
};

double percentile (const std::vector<double> &sorted, double ratio)
{
    if (sorted.empty ()) {
        return 0.0;
    }
    const auto index =
      std::min (sorted.size () - 1,
                static_cast<std::size_t> (std::ceil (sorted.size () * ratio)) - 1);
    return sorted[index];
}

benchmark_result_t run_burst (fixture_t &fixture, std::size_t packets_per_actor_for_run)
{
    const auto total_packets = actor_count * packets_per_actor_for_run;
    std::vector<double> latencies_us (total_packets);
    std::vector<std::thread> threads;
    threads.reserve (actor_count);

    const auto started = std::chrono::steady_clock::now ();
    for (std::size_t actor_index = 0; actor_index < actor_count; ++actor_index) {
        threads.emplace_back ([&, actor_index] {
            auto &actor = fixture.actors[actor_index];
            for (std::size_t packet = 0; packet < packets_per_actor_for_run; ++packet) {
                const auto packet_started = std::chrono::steady_clock::now ();
                auto result = fixture.context.handlers ().invoke_actor_packet (
                  "admission.move", fixture.spot, actor, fixture.services, fixture.serializers,
                  zlink::message_t::from (std::to_string (packet)));
                if (!result) {
                    std::cerr << "dispatch failed: " << result.error ()->what () << '\n';
                    std::abort ();
                }
                const auto packet_finished = std::chrono::steady_clock::now ();
                latencies_us[(actor_index * packets_per_actor_for_run) + packet] =
                  std::chrono::duration<double, std::micro> (packet_finished - packet_started)
                    .count ();
            }
        });
    }
    for (auto &thread : threads) {
        thread.join ();
    }
    const auto finished = std::chrono::steady_clock::now ();

    std::sort (latencies_us.begin (), latencies_us.end ());
    const auto wall_ms =
      std::chrono::duration<double, std::milli> (finished - started).count ();
    return benchmark_result_t{wall_ms,
                              total_packets / (wall_ms / 1000.0),
                              percentile (latencies_us, 0.50),
                              percentile (latencies_us, 0.95),
                              percentile (latencies_us, 0.99),
                              latencies_us.back ()};
}

double median (std::vector<double> values)
{
    std::sort (values.begin (), values.end ());
    return values[values.size () / 2];
}

} // namespace

int main (int argc, char **argv)
{
    bool serial = true;
    if (argc > 1 && std::string (argv[1]) == "--legacy") {
        serial = false;
    }

    fixture_t fixture (serial);
    (void) run_burst (fixture, warmup_packets / actor_count);

    std::vector<benchmark_result_t> results;
    results.reserve (rounds);
    for (std::size_t round = 0; round < rounds; ++round) {
        results.push_back (run_burst (fixture, packets_per_actor));
    }

    const auto expected_packets = (warmup_packets / actor_count * actor_count)
                                  + (actor_count * packets_per_actor * rounds);
    if (fixture.spot.admitted.load (std::memory_order_relaxed) != expected_packets) {
        std::cerr << "packet count mismatch: "
                  << fixture.spot.admitted.load (std::memory_order_relaxed) << " != "
                  << expected_packets << '\n';
        return 1;
    }

    const auto total_packets = actor_count * packets_per_actor;
    std::vector<double> walls;
    std::vector<double> throughputs;
    std::vector<double> p50s;
    std::vector<double> p95s;
    std::vector<double> p99s;
    for (const auto &result : results) {
        walls.push_back (result.wall_ms);
        throughputs.push_back (result.throughput);
        p50s.push_back (result.p50_us);
        p95s.push_back (result.p95_us);
        p99s.push_back (result.p99_us);
    }

    std::cout << "entry-spot admission burst ("
              << (serial ? "unified-serial" : "legacy-inline") << " dispatch)\n";
    std::cout << "actors=" << actor_count << " packetsPerActor=" << packets_per_actor
              << " totalPackets=" << total_packets << " rounds=" << rounds << '\n';
    for (std::size_t index = 0; index < results.size (); ++index) {
        const auto &result = results[index];
        std::cout << "round " << (index + 1) << ": wall=" << result.wall_ms
                  << "ms throughput=" << static_cast<std::uint64_t> (result.throughput)
                  << "/s p50=" << result.p50_us << "us p95=" << result.p95_us
                  << "us p99=" << result.p99_us << "us max=" << result.max_us << "us\n";
    }
    std::cout << "median: wall=" << median (walls)
              << "ms throughput=" << static_cast<std::uint64_t> (median (throughputs))
              << "/s p50=" << median (p50s) << "us p95=" << median (p95s)
              << "us p99=" << median (p99s) << "us\n";
    return 0;
}

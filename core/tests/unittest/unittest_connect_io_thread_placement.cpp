/* SPDX-License-Identifier: MPL-2.0 */

#include "../testutil_unity.hpp"
#include "../testutil.hpp"

#include "sockets/common/routed_submit_target.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "core/ctx.hpp"
#include "core/internal_defs.hpp"
#include "core/io_thread.hpp"
#include "core/session_base.hpp"
#include "sockets/common/socket_base.hpp"
#include "transports/tcp/asio_tcp_connecter.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
const size_t client_count = 100;
const int io_thread_count = 4;
const uint32_t no_thread = UINT32_MAX;

struct placement_t
{
    zlink::session_base_t *session;
    zlink::socket_base_t *socket;
    bool active;
    zlink::transport_lane_t lane;
    uint32_t session_thread;
    uint32_t connecter_thread;
    uint32_t engine_thread;
    bool handshaked;
};

std::mutex trace_sync;
std::map<zlink::session_base_t *, placement_t> placements;
std::map<const zlink::ctx_t *, size_t> selections;
std::map<std::thread::id, uint32_t> running_threads;
std::condition_variable threads_ready;
bool trace_only = false;
bool sequential = false;

}

// Observe the real non-LTO Core archive with GNU ld, without runtime hooks or
// production logging. Engine READY runs on the actual I/O executor, whose OS
// thread identity is recorded by posting once to each I/O context.
extern "C" zlink::session_base_t *
real_create (zlink::io_thread_t *,
             bool,
             zlink::socket_base_t *,
             const zlink::options_t &,
             zlink::address_t *) asm ("__real__ZN5zlink14session_base_t6createEPNS_11io_thread_"
                                      "tEbPNS_13socket_base_tERKNS_9options_tEPNS_9address_tE");
extern "C" zlink::session_base_t *wrap_create (
  zlink::io_thread_t *thread_,
  bool active_,
  zlink::socket_base_t *socket_,
  const zlink::options_t &options_,
  zlink::address_t *address_) asm ("__wrap__ZN5zlink14session_base_t6createEPNS_11io_thread_tEbPNS_"
                                   "13socket_base_tERKNS_9options_tEPNS_9address_tE");
extern "C" zlink::session_base_t *wrap_create (zlink::io_thread_t *thread_,
                                               bool active_,
                                               zlink::socket_base_t *socket_,
                                               const zlink::options_t &options_,
                                               zlink::address_t *address_)
{
    zlink::session_base_t *session = real_create (thread_, active_, socket_, options_, address_);
    std::lock_guard<std::mutex> lock (trace_sync);
    const placement_t placement = {
      session,   socket_,   active_, options_.transport_lane, thread_->get_tid (),
      no_thread, no_thread, false};
    placements[session] = placement;
    return session;
}

extern "C" void
real_connecter (zlink::asio_tcp_connecter_t *,
                zlink::io_thread_t *,
                zlink::session_base_t *,
                const zlink::options_t &,
                zlink::address_t *,
                bool) asm ("__real__ZN5zlink20asio_tcp_connecter_tC1EPNS_11io_thread_tEPNS_"
                           "14session_base_tERKNS_9options_tEPNS_9address_tEb");
extern "C" void
wrap_connecter (zlink::asio_tcp_connecter_t *,
                zlink::io_thread_t *,
                zlink::session_base_t *,
                const zlink::options_t &,
                zlink::address_t *,
                bool) asm ("__wrap__ZN5zlink20asio_tcp_connecter_tC1EPNS_11io_thread_tEPNS_"
                           "14session_base_tERKNS_9options_tEPNS_9address_tEb");
extern "C" void wrap_connecter (zlink::asio_tcp_connecter_t *connecter_,
                                zlink::io_thread_t *thread_,
                                zlink::session_base_t *session_,
                                const zlink::options_t &options_,
                                zlink::address_t *address_,
                                bool delayed_)
{
    real_connecter (connecter_, thread_, session_, options_, address_, delayed_);
    std::lock_guard<std::mutex> lock (trace_sync);
    placements.at (session_).connecter_thread = thread_->get_tid ();
}

extern "C" void
real_ready (zlink::session_base_t *) asm ("__real__ZN5zlink14session_base_t12engine_readyEv");
extern "C" void
wrap_ready (zlink::session_base_t *) asm ("__wrap__ZN5zlink14session_base_t12engine_readyEv");
extern "C" void wrap_ready (zlink::session_base_t *session_)
{
    real_ready (session_);
    std::lock_guard<std::mutex> lock (trace_sync);
    placement_t &placement = placements.at (session_);
    placement.lane = session_->transport_lane ();
    placement.engine_thread = running_threads.at (std::this_thread::get_id ());
    placement.handshaked = true;
}

extern "C" zlink::io_thread_t *
real_choose (const zlink::object_t *,
             uint64_t) asm ("__real__ZNK5zlink8object_t26choose_io_thread_transportEm");
extern "C" zlink::io_thread_t *
wrap_choose (const zlink::object_t *,
             uint64_t) asm ("__wrap__ZNK5zlink8object_t26choose_io_thread_transportEm");
extern "C" zlink::io_thread_t *wrap_choose (const zlink::object_t *owner_, uint64_t affinity_)
{
    zlink::io_thread_t *thread = real_choose (owner_, affinity_);
    std::lock_guard<std::mutex> lock (trace_sync);
    ++selections[owner_->get_ctx ()];
    return thread;
}

void setUp ()
{
    std::lock_guard<std::mutex> lock (trace_sync);
    placements.clear ();
    selections.clear ();
    running_threads.clear ();
}
void tearDown ()
{
}

namespace
{
void wait_ready (const std::vector<void *> &monitors_, std::vector<bool> &ready_)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (10);
    bool ready = false;
    do {
        ready = true;
        for (size_t i = 0; i != monitors_.size (); ++i) {
            if (!ready_[i]) {
                zlink_socket_monitor_event_t event = {};
                const zlink_recv_result_t result =
                  zlink_socket_monitor_recv (monitors_[i], &event, ZLINK_RECV_FLAGS_DONTWAIT);
                if (result == ZLINK_RECV_OK) {
                    TEST_ASSERT_EQUAL_UINT64 (ZLINK_EVENT_CONNECTION_READY, event.event);
                    ready_[i] = true;
                } else {
                    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, result);
                }
            }
            ready = ready && ready_[i];
        }
        if (ready) {
            std::lock_guard<std::mutex> lock (trace_sync);
            ready = placements.size () == monitors_.size () * 2;
            for (std::map<zlink::session_base_t *, placement_t>::const_iterator it =
                   placements.begin ();
                 it != placements.end (); ++it)
                ready = ready && it->second.handshaked;
        }
        if (ready)
            return;
        std::this_thread::yield ();
    } while (std::chrono::steady_clock::now () < deadline);
    {
        std::lock_guard<std::mutex> lock (trace_sync);
        printf ("NOT_READY,clients=%zu,sessions=%zu\n", monitors_.size (), placements.size ());
        for (std::map<zlink::session_base_t *, placement_t>::const_iterator it =
               placements.begin ();
             it != placements.end (); ++it) {
            const placement_t &p = it->second;
            printf ("PENDING,%d,%u,%u,%u,%d\n", p.active, p.session_thread, p.connecter_thread,
                    p.engine_thread, p.handshaked);
        }
    }
    TEST_FAIL_MESSAGE ("DEALER/ROUTER transport pairs did not become ready");
}

void check_dealer_router_connect_placement (bool check_choice_)
{
    zlink::ctx_t *client_ctx = new zlink::ctx_t;
    zlink::ctx_t *server_ctx = new zlink::ctx_t;
    TEST_ASSERT_SUCCESS_ERRNO (
      client_ctx->set (ZLINK_IO_THREADS, &io_thread_count, sizeof io_thread_count));
    TEST_ASSERT_SUCCESS_ERRNO (
      server_ctx->set (ZLINK_IO_THREADS, &io_thread_count, sizeof io_thread_count));
    zlink::socket_base_t *server =
      as_socket_handle (zlink_socket (server_ctx, ZLINK_SOCKET_ROUTER)).socket;
    TEST_ASSERT_NOT_NULL (server);
    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (server->setsockopt (ZLINK_INTERNAL_OPT_LINGER, &zero, sizeof zero));
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK,
                           zlink_bind (server->public_handle (), "tcp://127.0.0.1:*"));
    char endpoint[256];
    size_t endpoint_size = sizeof endpoint;
    TEST_ASSERT_SUCCESS_ERRNO (
      server->getsockopt (ZLINK_INTERNAL_OPT_LAST_ENDPOINT, endpoint, &endpoint_size));

    std::array<uint32_t, io_thread_count> thread_ids;
    std::vector<zlink::socket_base_t *> clients;
    std::vector<void *> monitors;
    std::vector<bool> ready;
    zlink::socket_base_t *first_client =
      as_socket_handle (zlink_socket (client_ctx, ZLINK_SOCKET_DEALER)).socket;
    TEST_ASSERT_NOT_NULL (first_client);
    for (size_t side = 0; side != 2; ++side) {
        zlink::ctx_t *ctx = side == 0 ? client_ctx : server_ctx;
        for (size_t i = 0; i != thread_ids.size (); ++i) {
            zlink::io_thread_t *thread = ctx->choose_io_thread (uint64_t (1) << i);
            TEST_ASSERT_NOT_NULL (thread);
            const uint32_t tid = thread->get_tid ();
            if (side == 0)
                thread_ids[i] = tid;
            boost::asio::post (thread->get_io_context (), [tid] () {
                std::lock_guard<std::mutex> lock (trace_sync);
                running_threads[std::this_thread::get_id ()] = tid;
                threads_ready.notify_one ();
            });
        }
    }
    {
        std::unique_lock<std::mutex> lock (trace_sync);
        TEST_ASSERT_TRUE (threads_ready.wait_for (lock, std::chrono::seconds (10), [] () {
            return running_threads.size () == 2 * io_thread_count;
        }));
    }
    for (size_t i = 0; i != client_count; ++i) {
        zlink::socket_base_t *client =
          i == 0 ? first_client
                 : as_socket_handle (zlink_socket (client_ctx, ZLINK_SOCKET_DEALER)).socket;
        TEST_ASSERT_NOT_NULL (client);
        TEST_ASSERT_SUCCESS_ERRNO (
          client->setsockopt (ZLINK_INTERNAL_OPT_LINGER, &zero, sizeof zero));
        char routing_id[32];
        const int length = snprintf (routing_id, sizeof routing_id, "client_%zu", i);
        TEST_ASSERT_SUCCESS_ERRNO (
          client->setsockopt (ZLINK_INTERNAL_OPT_ROUTING_ID, routing_id, length));
        clients.push_back (client);
        const zlink_socket_monitor_open_options_t monitor_options = {ZLINK_EVENT_CONNECTION_READY,
                                                                     0};
        void *monitor = zlink_socket_monitor_open (client->public_handle (), &monitor_options);
        TEST_ASSERT_NOT_NULL (monitor);
        monitors.push_back (monitor);
        ready.push_back (false);
        TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                               zlink_connect (client->public_handle (), endpoint));
        if (sequential)
            wait_ready (monitors, ready);
    }
    wait_ready (monitors, ready);

    // Snapshot while every connection is established; teardown may destroy
    // sessions, so retain values rather than dereferencing them afterwards.
    std::vector<placement_t> snapshot;
    size_t choice_count;
    {
        std::lock_guard<std::mutex> lock (trace_sync);
        for (std::map<zlink::session_base_t *, placement_t>::const_iterator it =
               placements.begin ();
             it != placements.end (); ++it)
            snapshot.push_back (it->second);
        choice_count = selections[client_ctx];
    }
    unsigned int counts[2][3][io_thread_count] = {};
    size_t mismatches = 0;
    size_t unexpected_lanes = 0;
    for (size_t i = 0; i != snapshot.size (); ++i) {
        const placement_t &p = snapshot[i];
        const size_t side = p.active ? 0 : 1;
        unexpected_lanes += p.lane != zlink::transport_lane_application;
        const uint32_t ids[] = {p.session_thread, p.connecter_thread, p.engine_thread};
        for (size_t object = 0; object != 3; ++object)
            for (size_t thread = 0; thread != thread_ids.size (); ++thread)
                if (ids[object] == thread_ids[thread])
                    ++counts[side][object][thread];
        if (p.active
            && (p.session_thread != p.connecter_thread || p.session_thread != p.engine_thread))
            ++mismatches;
        printf ("PLACEMENT,%s,%p,%p,%u,%u,%u,%u\n", p.active ? "client" : "server",
                static_cast<void *> (p.socket), static_cast<void *> (p.session),
                static_cast<unsigned int> (p.lane), p.session_thread, p.connecter_thread,
                p.engine_thread);
    }
    const char *objects[] = {"session", "connecter", "engine"};
    for (size_t side = 0; side != 2; ++side)
        for (size_t object = 0; object != 3; ++object)
            printf ("COUNTS,%s,application,%s,%u/%u/%u/%u\n", side == 0 ? "client" : "server",
                    objects[object], counts[side][object][0], counts[side][object][1],
                    counts[side][object][2], counts[side][object][3]);
    printf ("CHOICES,%zu,MISMATCHES,%zu\n", choice_count, mismatches);

    for (size_t i = 0; i != monitors.size (); ++i)
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_monitor_close (&monitors[i]));
    for (size_t i = 0; i != clients.size (); ++i)
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (clients[i]->public_handle ()));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (server->public_handle ()));
    client_ctx->terminate ();
    server_ctx->terminate ();

    if (!trace_only) {
        TEST_ASSERT_EQUAL_UINT (client_count * 2, snapshot.size ());
        TEST_ASSERT_EQUAL_UINT (0, unexpected_lanes);
        if (check_choice_) {
            TEST_ASSERT_EQUAL_UINT (0, mismatches);
            TEST_ASSERT_EQUAL_UINT (client_count, choice_count);
        } else {
            for (size_t thread = 0; thread != thread_ids.size (); ++thread) {
                for (size_t object = 0; object != 3; ++object)
                    TEST_ASSERT_EQUAL_UINT (client_count / io_thread_count,
                                            counts[0][object][thread]);
                TEST_ASSERT_EQUAL_UINT (client_count / io_thread_count, counts[1][0][thread]);
                TEST_ASSERT_EQUAL_UINT (client_count / io_thread_count, counts[1][2][thread]);
            }
        }
    }
}

void test_dealer_router_connect_distribution ()
{
    check_dealer_router_connect_placement (false);
}

void test_dealer_router_connect_selects_once ()
{
    check_dealer_router_connect_placement (true);
}
}

extern "C" int main (int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        trace_only = trace_only || strcmp (argv[i], "--trace-only") == 0;
        sequential = sequential || strcmp (argv[i], "--sequential") == 0;
    }
    UNITY_BEGIN ();
    RUN_TEST (test_dealer_router_connect_distribution);
    RUN_TEST (test_dealer_router_connect_selects_once);
    return UNITY_END ();
}

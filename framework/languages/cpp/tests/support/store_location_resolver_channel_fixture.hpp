/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include <zlink/json.hpp>
#include "runtime_failure_fixture.hpp"
#include "host_readiness.hpp"
#include "location_reclaim_clock.hpp"

#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/locations/location_auto_connect_host_service.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "runtime/channels/channel_runtime_manager.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/client_server/client_server_location_runtime.hpp"
#include "runtime/mesh/user_spot_terminal_mapping.hpp"
#include "runtime/streams/stream_runtime.hpp"
#include "runtime/streams/stream_host_service.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "owner_lease_time_store.hpp"

#include <gtest/gtest.h>
#include <zlink/framework.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace zlink::framework::test::store_location_resolvers
{
struct auto_connect_request_t
{
    // This is an ordinary application packet name. The ClientServer reply
    // header, not a reserved packet-name sentinel, determines success.
    static constexpr const char *packet_name = "$zlink.client-server.error";
    int value{};
};

struct auto_connect_reply_t
{
    int value{};
};

struct blocked_auto_connect_event_t
{
    static constexpr const char *packet_name = "blocked-auto-connect-event";
    int value{};
};

class auto_connect_request_handler_t
{
  public:
    using request_type = auto_connect_request_t;
    using reply_type = auto_connect_reply_t;

    auto_connect_reply_t handle (const auto_connect_request_t &request)
    {
        return auto_connect_reply_t{request.value + 1000};
    }
};

struct automatic_handler_scope_dependency_t
{
    automatic_handler_scope_dependency_t () { ++created; }
    ~automatic_handler_scope_dependency_t () { ++destroyed; }

    inline static std::atomic_int created{0};
    inline static std::atomic_int destroyed{0};
};

class automatic_handler_scope_filter_t
{
  public:
    explicit automatic_handler_scope_filter_t (automatic_handler_scope_dependency_t &dependency) :
        _dependency (dependency)
    {
    }

    zlink::framework::task_t<void>
    invoke (const zlink::framework::handler_filter_context_t &context,
            zlink::framework::handler_next_t next)
    {
        last_filter_dependency.store (&_dependency, std::memory_order_release);
        if (context.packet_name == blocked_auto_connect_event_t::packet_name) {
            ++blocked_fanout_dispatches;
            co_return;
        }
        if (reject_requests.load (std::memory_order_acquire))
            co_return;
        co_await next ();
        co_return;
    }

    inline static std::atomic_bool reject_requests{false};
    inline static std::atomic<void *> last_filter_dependency{nullptr};
    inline static std::atomic_int blocked_fanout_dispatches{0};

  private:
    automatic_handler_scope_dependency_t &_dependency;
};
} // namespace zlink::framework::test::store_location_resolvers

template <>
struct zlink::json::dto_traits<
  zlink::framework::test::store_location_resolvers::auto_connect_request_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "value", &zlink::framework::test::store_location_resolvers::auto_connect_request_t::value)};
};

template <>
struct zlink::json::dto_traits<
  zlink::framework::test::store_location_resolvers::auto_connect_reply_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "value", &zlink::framework::test::store_location_resolvers::auto_connect_reply_t::value)};
};

template <>
struct zlink::json::dto_traits<
  zlink::framework::test::store_location_resolvers::blocked_auto_connect_event_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "value",
      &zlink::framework::test::store_location_resolvers::blocked_auto_connect_event_t::value)};
};

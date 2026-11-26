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
struct lease_target_request_t
{
    static constexpr const char *packet_name = "lease-target-request";
    int value{};
};

struct lease_target_reply_t
{
    std::string node_rid;
};

struct user_spot_delivery_probe_t
{
    static constexpr const char *packet_name = "user-spot-delivery-probe";
    int value{};
};

class actor_free_user_spot_contract_t : public zlink::framework::spot_t<zlink::framework::actor_t>
{
  public:
    zlink::framework::task_t<zlink::framework::spot_actor_join_result_t>
    on_actor_join (std::string_view, const zlink::framework::message_t &) override
    {
        co_return zlink::framework::spot_actor_join_result_t::reject ();
    }
    zlink::framework::task_t<void> on_actor_joined (zlink::framework::actor_t &) override
    {
        co_return;
    }
    zlink::framework::task_t<void> on_leave_actor (zlink::framework::actor_t &) override
    {
        co_return;
    }
};

class local_user_spot_t final : public actor_free_user_spot_contract_t
{
  public:
    explicit local_user_spot_t (zlink::framework::spot_context_t context) :
        _context (std::move (context))
    {
    }
    zlink::framework::spot_context_t &context () noexcept override { return _context; }
    const zlink::framework::spot_context_t &context () const noexcept override { return _context; }
    void configure () override
    {
        _context.handlers ().add_handler<&local_user_spot_t::handle> (
          user_spot_delivery_probe_t::packet_name);
    }

    void handle (const user_spot_delivery_probe_t &message)
    {
        last_delivery.store (message.value, std::memory_order_release);
    }

    static inline std::atomic_int last_delivery{0};

  private:
    zlink::framework::spot_context_t _context;
};

class context_owned_user_spot_t final : public actor_free_user_spot_contract_t
{
  public:
    explicit context_owned_user_spot_t (zlink::framework::spot_context_t context,
                                        std::shared_ptr<std::atomic<int>> destruction_count = {}) :
        _context (std::move (context)), _destruction_count (std::move (destruction_count))
    {
    }

    ~context_owned_user_spot_t () override
    {
        if (_destruction_count)
            _destruction_count->fetch_add (1, std::memory_order_relaxed);
    }

    zlink::framework::spot_context_t &context () noexcept override { return _context; }
    const zlink::framework::spot_context_t &context () const noexcept override { return _context; }
    void configure () override {}

  private:
    zlink::framework::spot_context_t _context;
    std::shared_ptr<std::atomic<int>> _destruction_count;
};
} // namespace zlink::framework::test::store_location_resolvers

template <>
struct zlink::json::dto_traits<
  zlink::framework::test::store_location_resolvers::lease_target_request_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "value", &zlink::framework::test::store_location_resolvers::lease_target_request_t::value)};
};

template <>
struct zlink::json::dto_traits<
  zlink::framework::test::store_location_resolvers::lease_target_reply_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "nodeRid",
      &zlink::framework::test::store_location_resolvers::lease_target_reply_t::node_rid)};
};

template <>
struct zlink::json::dto_traits<
  zlink::framework::test::store_location_resolvers::user_spot_delivery_probe_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "value",
      &zlink::framework::test::store_location_resolvers::user_spot_delivery_probe_t::value)};
};

/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "runtime/locations/spot_address_resolvers.hpp"
#include <zlink/framework/contracts/spots/spot.hpp>
#include <zlink/json.hpp>
#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace framework_instance_spot_activation_fixture
{
struct event_t
{
    static constexpr const char *packet_name = "instance.event";
    int value{};
};

struct request_t
{
    static constexpr const char *packet_name = "instance.request";
    int value{};
};

struct reply_t
{
    int value{};
};

struct traced_event_t
{
    static constexpr const char *packet_name = "instance.traced.event";
    int value{};
};

struct traced_request_t
{
    static constexpr const char *packet_name = "instance.traced.request";
    int value{};
};

struct traced_reply_t
{
    int value{};
};
} // namespace framework_instance_spot_activation_fixture

template <> struct zlink::json::dto_traits<framework_instance_spot_activation_fixture::event_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("value", &framework_instance_spot_activation_fixture::event_t::value)};
};

template <> struct zlink::json::dto_traits<framework_instance_spot_activation_fixture::request_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("value", &framework_instance_spot_activation_fixture::request_t::value)};
};

template <> struct zlink::json::dto_traits<framework_instance_spot_activation_fixture::reply_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("value", &framework_instance_spot_activation_fixture::reply_t::value)};
};

template <>
struct zlink::json::dto_traits<framework_instance_spot_activation_fixture::traced_event_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "value", &framework_instance_spot_activation_fixture::traced_event_t::value)};
};

template <>
struct zlink::json::dto_traits<framework_instance_spot_activation_fixture::traced_request_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "value", &framework_instance_spot_activation_fixture::traced_request_t::value)};
};

template <>
struct zlink::json::dto_traits<framework_instance_spot_activation_fixture::traced_reply_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "value", &framework_instance_spot_activation_fixture::traced_reply_t::value)};
};

namespace framework_instance_spot_activation_fixture
{
class resolver_t final : public zlink::framework::runtime::spot_address_resolver_t
{
  public:
    zlink::framework::task_t<std::optional<zlink::framework::runtime::spot_address_t>>
    resolve_spot_address (std::string, std::string spot_id) override
    {
        ++reads;
        if (lookup_gate)
            co_await lookup_gate->task ();
        const auto found = addresses.find (spot_id);
        co_return found == addresses.end ()
          ? std::nullopt
          : std::optional<zlink::framework::runtime::spot_address_t> (found->second);
    }

    void invalidate_spot_address (std::string_view spot_id) override
    {
        addresses.erase (std::string (spot_id));
    }

    void invalidate_all_routes_after_store_recovery () override { addresses.clear (); }

    std::atomic_int reads{0};
    std::shared_ptr<zlink::framework::task_completion_source_t<void>> lookup_gate;
    std::map<std::string, zlink::framework::runtime::spot_address_t> addresses;
};

class traced_instance_spot_t final : public zlink::framework::instance_spot_t
{
  public:
    explicit traced_instance_spot_t (zlink::framework::instance_spot_context_t context) :
        _context (std::move (context))
    {
    }

    zlink::framework::instance_spot_context_t &context () noexcept override { return _context; }

    const zlink::framework::instance_spot_context_t &context () const noexcept override
    {
        return _context;
    }

    void configure () override
    {
        _context.handlers ().add_handler<&traced_instance_spot_t::on_event> ();
        _context.handlers ().add_handler<&traced_instance_spot_t::on_request> ();
    }

    void on_event (const traced_event_t &event) { last_event = event.value; }

    traced_reply_t on_request (const traced_request_t &request) { return {request.value + 1}; }

    int last_event = 0;

  private:
    zlink::framework::instance_spot_context_t _context;
};
} // namespace framework_instance_spot_activation_fixture

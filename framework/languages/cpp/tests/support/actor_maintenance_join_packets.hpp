/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include <zlink/json.hpp>
#include <string>
#include <tuple>

namespace zlink::framework::test::actor_maintenance_join
{
struct move_t
{
    static constexpr const char *packet_name = "maintenance-join-move";
    std::string target;
};

struct probe_t
{
    static constexpr const char *packet_name = "maintenance-join-probe";
    int value{};
};

struct close_session_t
{
    static constexpr const char *packet_name = "bound-session-close";
    int value{};
};

struct ready_t
{
    static constexpr const char *packet_name = "maintenance-join-ready";
    int value{};
};

struct view_request_t
{
    static constexpr const char *packet_name = "maintenance-join-view-request";
    int value{};
};

struct view_t
{
    static constexpr const char *packet_name = "maintenance-join-view";
    int instance{};
};

struct record_t
{
    static constexpr const char *packet_name = "maintenance-join-record";
    int value{};
};
} // namespace zlink::framework::test::actor_maintenance_join

template <> struct zlink::json::dto_traits<zlink::framework::test::actor_maintenance_join::move_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "target", &zlink::framework::test::actor_maintenance_join::move_t::target)};
};

template <> struct zlink::json::dto_traits<zlink::framework::test::actor_maintenance_join::probe_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "value", &zlink::framework::test::actor_maintenance_join::probe_t::value)};
};

template <>
struct zlink::json::dto_traits<zlink::framework::test::actor_maintenance_join::close_session_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "value", &zlink::framework::test::actor_maintenance_join::close_session_t::value)};
};

template <> struct zlink::json::dto_traits<zlink::framework::test::actor_maintenance_join::ready_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "value", &zlink::framework::test::actor_maintenance_join::ready_t::value)};
};

template <>
struct zlink::json::dto_traits<zlink::framework::test::actor_maintenance_join::view_request_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "value", &zlink::framework::test::actor_maintenance_join::view_request_t::value)};
};

template <> struct zlink::json::dto_traits<zlink::framework::test::actor_maintenance_join::view_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "instance", &zlink::framework::test::actor_maintenance_join::view_t::instance)};
};

template <> struct zlink::json::dto_traits<zlink::framework::test::actor_maintenance_join::record_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "value", &zlink::framework::test::actor_maintenance_join::record_t::value)};
};

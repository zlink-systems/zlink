/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework.hpp>

#include <stdexcept>
#include <string>

namespace zlink::e2e::to_actor_messaging
{

struct redis_configuration_t
{
    std::string endpoint;
    std::string key_prefix;

    static redis_configuration_t bind (const framework::configuration_section_t &section)
    {
        return {.endpoint = section.require ("endpoint"),
                .key_prefix = section.require ("keyPrefix")};
    }
};

struct actor_configuration_t
{
    redis_configuration_t redis;
    std::string log_dir;
    std::string node_rid;
    std::string http_endpoint;
    std::string spot_endpoint;
    std::string pub_sub_endpoint;
    std::string caller_rid;
    std::string caller_spot_endpoint;

    static actor_configuration_t bind (const framework::configuration_section_t &section)
    {
        return {.redis = {.endpoint = section.require ("redis.endpoint"),
                          .key_prefix = section.require ("redis.keyPrefix")},
                .log_dir = section.require ("logDir"),
                .node_rid = section.require ("nodeRid"),
                .http_endpoint = section.require ("httpEndpoint"),
                .spot_endpoint = section.require ("spotEndpoint"),
                .pub_sub_endpoint = section.require ("pubSubEndpoint"),
                .caller_rid = section.require ("callerRid"),
                .caller_spot_endpoint = section.require ("callerSpotEndpoint")};
    }
};

struct caller_configuration_t
{
    redis_configuration_t redis;
    std::string log_dir;
    std::string node_rid;
    std::string http_endpoint;
    std::string spot_endpoint;
    std::string pub_sub_endpoint;
    std::string actor_rid;
    std::string actor_spot_endpoint;
    std::string actor_b_rid;
    std::string actor_b_spot_endpoint;

    static caller_configuration_t bind (const framework::configuration_section_t &section)
    {
        return {.redis = {.endpoint = section.require ("redis.endpoint"),
                          .key_prefix = section.require ("redis.keyPrefix")},
                .log_dir = section.require ("logDir"),
                .node_rid = section.require ("nodeRid"),
                .http_endpoint = section.require ("httpEndpoint"),
                .spot_endpoint = section.require ("spotEndpoint"),
                .pub_sub_endpoint = section.require ("pubSubEndpoint"),
                .actor_rid = section.require ("actorRid"),
                .actor_spot_endpoint = section.require ("actorSpotEndpoint"),
                .actor_b_rid = section.require ("actorBRid"),
                .actor_b_spot_endpoint = section.require ("actorBSpotEndpoint")};
    }
};

struct session_configuration_t
{
    redis_configuration_t redis;
    std::string log_dir;
    std::string node_rid;
    std::string http_endpoint;
    std::string stream_endpoint;
    std::string spot_endpoint;
    std::string pub_sub_endpoint;
    std::string actor_rid;
    std::string actor_spot_endpoint;

    static session_configuration_t bind (const framework::configuration_section_t &section)
    {
        return {.redis = {.endpoint = section.require ("redis.endpoint"),
                          .key_prefix = section.require ("redis.keyPrefix")},
                .log_dir = section.require ("logDir"),
                .node_rid = section.require ("nodeRid"),
                .http_endpoint = section.require ("httpEndpoint"),
                .stream_endpoint = section.require ("streamEndpoint"),
                .spot_endpoint = section.require ("spotEndpoint"),
                .pub_sub_endpoint = section.require ("pubSubEndpoint"),
                .actor_rid = section.require ("actorRid"),
                .actor_spot_endpoint = section.require ("actorSpotEndpoint")};
    }
};

template<typename T>
T load_role_configuration (framework::app_t &app, int argc, char **argv)
{
    app.config ().load_cli (argc, argv);
    const auto path = app.config ().model ().get ("config");
    if (!path) {
        throw std::runtime_error ("ToActorMessaging server role requires --config=<path>");
    }
    app.config ().load_json (*path);
    return app.config ().bind_required<T> ("e2e");
}

} // namespace zlink::e2e::to_actor_messaging

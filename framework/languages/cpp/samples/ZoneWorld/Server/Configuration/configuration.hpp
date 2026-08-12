/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework.hpp>

#include <stdexcept>
#include <string>

namespace zlink::samples::zoneworld
{

struct configuration_t
{
    std::string redis_endpoint;
    std::string redis_key_prefix;
    std::string node_id;
    std::string mesh_endpoint;
    std::string stream_endpoint;
    std::string broadcast_endpoint;
    std::string bootstrap_http_endpoint;
    std::string log_dir;

    static configuration_t bind (
      const zlink::framework::configuration_section_t &section)
    {
        return {
          section.require ("redisEndpoint"),
          section.require ("redisKeyPrefix"),
          section.require ("nodeId"),
          section.require ("meshEndpoint"),
          section.require ("streamEndpoint"),
          section.require ("broadcastEndpoint"),
          section.require ("bootstrapHttpEndpoint"),
          section.require ("logDir")};
    }
};

inline configuration_t load_configuration (
  zlink::framework::app_t &app,
  int argc,
  char **argv)
{
    app.config ().load_cli (argc, argv);
    const auto path = app.config ().model ().get ("config");
    if (!path) {
        throw std::runtime_error (
          "ZoneWorld role requires --config=<path>");
    }
    app.config ().load_json (*path);
    return app.config ().bind_required<configuration_t> (
      "sample.zoneworld");
}

} // namespace zlink::samples::zoneworld

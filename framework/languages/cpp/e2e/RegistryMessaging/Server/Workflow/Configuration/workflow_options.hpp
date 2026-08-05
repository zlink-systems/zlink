/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework.hpp>
#include <string>

namespace zlink::framework::e2e::registry_messaging::workflow
{

struct workflow_options_t
{
    std::string rid;
    std::string instance_id;
    std::string workflow_endpoint;
    std::string http_endpoint;
    std::string redis_endpoint;
    std::string redis_key_prefix;
    std::string log_dir;
    static workflow_options_t bind (const configuration_section_t &section)
    {
        const auto rid = section.get ("rid").value_or ("workflow-a");
        return {.rid = rid,
                .instance_id = section.get ("instanceId").value_or (rid),
                .workflow_endpoint = section.require ("workflowEndpoint"),
                .http_endpoint = section.require ("httpEndpoint"),
                .redis_endpoint = section.require ("redis.endpoint"),
                .redis_key_prefix = section.require ("redis.keyPrefix"),
                .log_dir = section.require ("logDir")};
    }
};

} // namespace zlink::framework::e2e::registry_messaging::workflow

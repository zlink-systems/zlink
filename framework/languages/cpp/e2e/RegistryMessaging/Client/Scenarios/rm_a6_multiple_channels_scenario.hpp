/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../Support/client_support.hpp"

#include <iostream>

namespace zlink::framework::e2e::registry_messaging::client
{

inline void run_rm_a6_multiple_channels_scenario (const client_options_t &options)
{
    const auto consumer_url = options.store_consumer_url;
    auto api = post_json<profile_req_t, profile_res_t> (
      consumer_url, "/profile/request",
      profile_req_t{.value = "a6-api-before"});
    ensure (api.provider_rid.rfind ("api-", 0) == 0,
            "RM-A6 api channel resolved a non-api provider");

    auto workflow = post_json<workflow_req_t, workflow_res_t> (
      consumer_url, "/workflow/request",
      workflow_req_t{.value = "a6-workflow-before"});
    ensure (workflow.value == "workflow:a6-workflow-before",
            "RM-A6 workflow reply value mismatch");
    ensure (workflow.provider_rid == "workflow-a",
            "RM-A6 workflow channel resolved the wrong provider");

    const auto evidence_a = fetch_evidence (options.http_a_endpoint);
    const auto evidence_b = fetch_evidence (options.http_b_endpoint);
    const auto workflow_evidence =
      fetch_evidence (options.http_workflow_endpoint);
    bool workflow_recorded = false;
    bool workflow_leaked_to_provider = false;
    bool profile_leaked_to_workflow = false;
    for (const auto &entry : workflow_evidence.entries) {
        if (entry.marker == "WorkflowReq"
            && entry.value == "a6-workflow-before") {
            workflow_recorded = true;
        }
        if (entry.marker == "ProfileReq"
            && entry.value == "a6-api-before") {
            profile_leaked_to_workflow = true;
        }
    }
    for (const auto &snapshot : {evidence_a, evidence_b}) {
        for (const auto &entry : snapshot.entries) {
            if (entry.marker == "WorkflowReq"
                && entry.value == "a6-workflow-before") {
                workflow_leaked_to_provider = true;
            }
        }
    }
    ensure (
      workflow_recorded,
      "RM-A6 workflow evidence was not recorded");
    ensure (
      !workflow_leaked_to_provider,
      "RM-A6 workflow request reached a profile provider");
    ensure (
      !profile_leaked_to_workflow,
      "RM-A6 profile request reached the workflow provider");

    touch_file (options.ready_file);
    wait_for_file (options.continue_file, options.control_wait);

    auto api_after_profile_scale_in =
      post_json<profile_req_t, profile_res_t> (
        consumer_url, "/profile/request",
        profile_req_t{.value = "a6-api-after-profile-scale-in"});
    ensure (
      api_after_profile_scale_in.provider_rid == "api-a",
      "RM-A6 profile scale-in did not retain the remaining api provider");
    auto workflow_after_profile_scale_in =
      post_json<workflow_req_t, workflow_res_t> (
        consumer_url, "/workflow/request",
        workflow_req_t{
          .value = "a6-workflow-after-profile-scale-in"});
    ensure (
      workflow_after_profile_scale_in.provider_rid == "workflow-a",
      "RM-A6 profile scale-in affected workflow routing");

    touch_file (options.ready_file + ".workflow");
    wait_for_file (
      options.continue_file + ".workflow",
      options.control_wait);

    auto api_after_workflow_scale_in =
      post_json<profile_req_t, profile_res_t> (
        consumer_url, "/profile/request",
        profile_req_t{.value = "a6-api-after-workflow-scale-in"});
    ensure (
      api_after_workflow_scale_in.provider_rid == "api-a",
      "RM-A6 workflow scale-in affected profile routing");

    std::cout << "scenario RM-A6 passed\n";
}

} // namespace zlink::framework::e2e::registry_messaging::client

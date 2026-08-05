/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/pubsub_contracts.hpp"

#include <zlink/framework.hpp>

#include <chrono>
#include <csignal>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace zlink::framework::e2e::pubsub::server
{

inline bool has_topic (const std::string &topics, const std::string &topic)
{
    return ("," + topics + ",").find ("," + topic + ",") != std::string::npos;
}

inline void configure_codecs (zlink::framework::codec_options_builder_t codecs)
{
}

inline std::string kind_name (zlink::framework::dispatch_message_kind_t value)
{
    switch (value) {
        case zlink::framework::dispatch_message_kind_t::publish:
            return "publish";
        case zlink::framework::dispatch_message_kind_t::send:
            return "send";
        case zlink::framework::dispatch_message_kind_t::request:
            return "request";
        default:
            return "other";
    }
}

inline std::string reason_name (zlink::framework::dispatch_error_reason_t value)
{
    switch (value) {
        case zlink::framework::dispatch_error_reason_t::handler_missing:
            return "handlerMissing";
        case zlink::framework::dispatch_error_reason_t::payload_decode_failed:
            return "payloadDecodeFailed";
        case zlink::framework::dispatch_error_reason_t::handler_exception:
            return "handlerException";
        default:
            return "other";
    }
}

inline std::string action_name (zlink::framework::dispatch_error_action_t value)
{
    return value == zlink::framework::dispatch_error_action_t::drop ? "drop" : "replyError";
}

inline void configure_flow (zlink::framework::zlink_framework_options_t &options,
                            const std::string &log_dir,
                            const std::string &label)
{
    options.configure_dispatch ()
      .message_flow (zlink::framework::message_flow_log_mode_t::key_transitions)
      .trace_log_file (log_dir + "/" + label + "-flow.log")
      .trace_label ("cpp-ps-" + label);
}

class operational_evidence_store_t
{
  public:
    explicit operational_evidence_store_t (std::string rid) : _rid (std::move (rid)) {}

    void add (std::string entry)
    {
        std::lock_guard lock (_mutex);
        _entries.push_back (std::move (entry));
    }

    nlohmann::json snapshot () const
    {
        std::lock_guard lock (_mutex);
        return nlohmann::json{{"rid", _rid}, {"entries", _entries}};
    }

    void clear ()
    {
        std::lock_guard lock (_mutex);
        _entries.clear ();
    }

  private:
    std::string _rid;
    mutable std::mutex _mutex;
    std::vector<std::string> _entries;
};

class operational_evidence_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<operational_evidence_store_t>;

    explicit operational_evidence_handler_t (operational_evidence_store_t &evidence) :
        _evidence (evidence)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        zlink::framework::http_response_t response;
        response.body = _evidence.snapshot ().dump ();
        return response;
    }

  private:
    operational_evidence_store_t &_evidence;
};

class operational_evidence_clear_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<operational_evidence_store_t>;

    explicit operational_evidence_clear_handler_t (operational_evidence_store_t &evidence) :
        _evidence (evidence)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        _evidence.clear ();
        zlink::framework::http_response_t response;
        response.body = R"({"status":"cleared"})";
        return response;
    }

  private:
    operational_evidence_store_t &_evidence;
};

class operational_shutdown_handler_t
{
  public:
    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        std::thread ([] {
            std::this_thread::sleep_for (std::chrono::milliseconds (20));
            std::raise (SIGTERM);
        }).detach ();
        zlink::framework::http_response_t response;
        response.body = R"({"status":"stopping"})";
        return response;
    }
};

} // namespace zlink::framework::e2e::pubsub::server

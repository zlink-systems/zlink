/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <nlohmann/json.hpp>

#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace zlink::framework::e2e::submit_admission
{

inline constexpr const char *mesh_name = "submit.admission.mesh";
inline constexpr const char *channel_name = "submit.admission.channel";
inline constexpr const char *fanout_channel = "submit.admission.fanout";
inline constexpr const char *client_server_channel = "submit.admission.client-server";
inline constexpr const char *client_server_handler_group =
  "submit.admission.client-server.handlers";

struct admission_message_t
{
    static constexpr const char *packet_name = "admission";
    std::string operation_id;
    std::uint64_t sequence = 0;
    std::string payload;
};

struct node_submit_request_t
{
    std::string target_rid;
    admission_message_t message;
};

struct submit_response_t
{
    std::string operation_id;
    std::string status;
    std::uint64_t public_invocation_count = 0;
    std::uint64_t terminal_count = 0;
};

struct actor_target_t
{
    std::string operation_id;
    std::string actor_id;
    std::string node_rid;
    std::uint64_t generation = 0;
};

struct actor_relay_request_t
{
    std::string actor_id;
    admission_message_t message;
};

struct operation_evidence_t
{
    std::string operation_id;
    std::uint64_t handler_entered_count = 0;
    std::uint64_t handler_completed_count = 0;
};

inline void to_json (nlohmann::json &json, const admission_message_t &value)
{
    json = nlohmann::json{{"operationId", value.operation_id},
                          {"sequence", value.sequence},
                          {"payload", value.payload}};
}

inline void from_json (const nlohmann::json &json, admission_message_t &value)
{
    json.at ("operationId").get_to (value.operation_id);
    json.at ("sequence").get_to (value.sequence);
    json.at ("payload").get_to (value.payload);
}

inline void to_json (nlohmann::json &json, const node_submit_request_t &value)
{
    json = nlohmann::json{{"targetRid", value.target_rid}, {"message", value.message}};
}

inline void from_json (const nlohmann::json &json, node_submit_request_t &value)
{
    json.at ("targetRid").get_to (value.target_rid);
    json.at ("message").get_to (value.message);
}

inline void to_json (nlohmann::json &json, const submit_response_t &value)
{
    json = nlohmann::json{{"operationId", value.operation_id},
                          {"status", value.status},
                          {"publicInvocationCount", value.public_invocation_count},
                          {"terminalCount", value.terminal_count}};
}

inline void from_json (const nlohmann::json &json, submit_response_t &value)
{
    json.at ("operationId").get_to (value.operation_id);
    json.at ("status").get_to (value.status);
    json.at ("publicInvocationCount").get_to (value.public_invocation_count);
    json.at ("terminalCount").get_to (value.terminal_count);
}

inline void to_json (nlohmann::json &json, const actor_target_t &value)
{
    json = nlohmann::json{{"operationId", value.operation_id},
                          {"actorId", value.actor_id},
                          {"nodeRid", value.node_rid},
                          {"generation", value.generation}};
}

inline void from_json (const nlohmann::json &json, actor_target_t &value)
{
    json.at ("operationId").get_to (value.operation_id);
    json.at ("actorId").get_to (value.actor_id);
    json.at ("nodeRid").get_to (value.node_rid);
    json.at ("generation").get_to (value.generation);
}

inline void to_json (nlohmann::json &json, const actor_relay_request_t &value)
{
    json = nlohmann::json{{"actorId", value.actor_id}, {"message", value.message}};
}

inline void from_json (const nlohmann::json &json, actor_relay_request_t &value)
{
    json.at ("actorId").get_to (value.actor_id);
    json.at ("message").get_to (value.message);
}

inline void to_json (nlohmann::json &json, const operation_evidence_t &value)
{
    json = nlohmann::json{{"operationId", value.operation_id},
                          {"handlerEnteredCount", value.handler_entered_count},
                          {"handlerCompletedCount", value.handler_completed_count}};
}

class handler_gate_t
{
  public:
    void close ()
    {
        std::lock_guard lock (_mutex);
        _open = false;
    }

    void open ()
    {
        {
            std::lock_guard lock (_mutex);
            _open = true;
        }
        _changed.notify_all ();
    }

    void wait ()
    {
        std::unique_lock lock (_mutex);
        _changed.wait (lock, [&] { return _open; });
    }

  private:
    std::mutex _mutex;
    std::condition_variable _changed;
    bool _open = true;
};

class evidence_store_t
{
  public:
    void entered (const std::string &operation_id)
    {
        std::lock_guard lock (_mutex);
        auto &entry = _entries[operation_id];
        entry.operation_id = operation_id;
        ++entry.handler_entered_count;
    }

    void completed (const std::string &operation_id)
    {
        std::lock_guard lock (_mutex);
        auto &entry = _entries[operation_id];
        entry.operation_id = operation_id;
        ++entry.handler_completed_count;
    }

    operation_evidence_t get (const std::string &operation_id) const
    {
        std::lock_guard lock (_mutex);
        const auto found = _entries.find (operation_id);
        return found == _entries.end () ? operation_evidence_t{operation_id} : found->second;
    }

  private:
    mutable std::mutex _mutex;
    std::map<std::string, operation_evidence_t> _entries;
};

} // namespace zlink::framework::e2e::submit_admission

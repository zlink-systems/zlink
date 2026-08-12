/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
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

struct admission_msg_t
{
    static constexpr const char *packet_name = "AdmissionMsg";
    std::string operation_id;
    std::uint64_t sequence = 0;
    std::string payload;
};

struct admission_req_t
{
    static constexpr const char *packet_name = "AdmissionReq";
    std::string operation_id;
    std::uint64_t sequence = 0;
    std::string payload;
};

struct admission_event_t
{
    static constexpr const char *packet_name = "AdmissionEvent";
    std::string operation_id;
    std::uint64_t sequence = 0;
    std::string payload;
};

struct admission_res_t
{
    static constexpr const char *packet_name = "AdmissionRes";
    std::string operation_id;
    std::string status;
    std::uint64_t public_invocation_count = 0;
    std::uint64_t terminal_count = 0;
};

struct saturation_prime_msg_t
{
    static constexpr const char *packet_name = "SaturationPrimeMsg";
    std::string operation_id;
    std::uint64_t sequence = 0;
    std::string payload;
};

struct node_submit_req_t
{
    std::string target_rid;
    admission_msg_t message;
};

struct actor_ensure_req_t
{
    std::string operation_id;
    std::string actor_id;
};

struct actor_ensure_res_t
{
    std::string operation_id;
    std::string actor_id;
    std::string node_rid;
    std::uint64_t generation = 0;
};

struct actor_create_req_t
{
    static constexpr const char *packet_name = "AdmissionActorCreateReq";
    std::string actor_id;
};

struct actor_bind_req_t
{
    static constexpr const char *packet_name = "ActorBindReq";
    std::string operation_id;
    std::string actor_id;
    std::string node_rid;
    std::uint64_t generation = 0;
};

struct actor_bind_res_t
{
    static constexpr const char *packet_name = "ActorBindRes";
    std::string operation_id;
    std::string actor_id;
    std::string node_rid;
    std::uint64_t generation = 0;
};

struct actor_relay_req_t
{
    static constexpr const char *packet_name = "ActorRelayReq";
    std::string actor_id;
    admission_msg_t message;
};

struct operation_evidence_t
{
    std::string operation_id;
    std::uint64_t handler_entered_count = 0;
    std::uint64_t handler_completed_count = 0;
};

template <typename T> inline void write_admission_payload (nlohmann::json &json, const T &value)
{
    json = nlohmann::json{{"operationId", value.operation_id},
                          {"sequence", value.sequence},
                          {"payload", value.payload}};
}

template <typename T> inline void read_admission_payload (const nlohmann::json &json, T &value)
{
    json.at ("operationId").get_to (value.operation_id);
    json.at ("sequence").get_to (value.sequence);
    json.at ("payload").get_to (value.payload);
}

inline void to_json (nlohmann::json &json, const admission_msg_t &value)
{ write_admission_payload (json, value); }
inline void from_json (const nlohmann::json &json, admission_msg_t &value)
{ read_admission_payload (json, value); }
inline void to_json (nlohmann::json &json, const admission_req_t &value)
{ write_admission_payload (json, value); }
inline void from_json (const nlohmann::json &json, admission_req_t &value)
{ read_admission_payload (json, value); }
inline void to_json (nlohmann::json &json, const admission_event_t &value)
{ write_admission_payload (json, value); }
inline void from_json (const nlohmann::json &json, admission_event_t &value)
{ read_admission_payload (json, value); }
inline void to_json (nlohmann::json &json, const saturation_prime_msg_t &value)
{ write_admission_payload (json, value); }
inline void from_json (const nlohmann::json &json, saturation_prime_msg_t &value)
{ read_admission_payload (json, value); }

inline void to_json (nlohmann::json &json, const node_submit_req_t &value)
{
    json = nlohmann::json{{"targetRid", value.target_rid}, {"message", value.message}};
}

inline void from_json (const nlohmann::json &json, node_submit_req_t &value)
{
    json.at ("targetRid").get_to (value.target_rid);
    json.at ("message").get_to (value.message);
}

inline void to_json (nlohmann::json &json, const admission_res_t &value)
{
    json = nlohmann::json{{"operationId", value.operation_id},
                          {"status", value.status},
                          {"publicInvocationCount", value.public_invocation_count},
                          {"terminalCount", value.terminal_count}};
}

inline void from_json (const nlohmann::json &json, admission_res_t &value)
{
    json.at ("operationId").get_to (value.operation_id);
    json.at ("status").get_to (value.status);
    json.at ("publicInvocationCount").get_to (value.public_invocation_count);
    json.at ("terminalCount").get_to (value.terminal_count);
}

template <typename T> inline void write_actor_target (nlohmann::json &json, const T &value)
{
    json = nlohmann::json{{"operationId", value.operation_id},
                          {"actorId", value.actor_id},
                          {"nodeRid", value.node_rid},
                          {"generation", value.generation}};
}

template <typename T> inline void read_actor_target (const nlohmann::json &json, T &value)
{
    json.at ("operationId").get_to (value.operation_id);
    json.at ("actorId").get_to (value.actor_id);
    json.at ("nodeRid").get_to (value.node_rid);
    json.at ("generation").get_to (value.generation);
}

inline void to_json (nlohmann::json &json, const actor_ensure_req_t &value)
{
    json = nlohmann::json{{"operationId", value.operation_id},
                          {"actorId", value.actor_id}};
}
inline void from_json (const nlohmann::json &json, actor_ensure_req_t &value)
{
    json.at ("operationId").get_to (value.operation_id);
    json.at ("actorId").get_to (value.actor_id);
}
inline void to_json (nlohmann::json &json, const actor_ensure_res_t &value)
{ write_actor_target (json, value); }
inline void from_json (const nlohmann::json &json, actor_ensure_res_t &value)
{ read_actor_target (json, value); }
inline void to_json (nlohmann::json &json, const actor_create_req_t &value)
{ json = nlohmann::json{{"actorId", value.actor_id}}; }
inline void from_json (const nlohmann::json &json, actor_create_req_t &value)
{ json.at ("actorId").get_to (value.actor_id); }
inline void to_json (nlohmann::json &json, const actor_bind_req_t &value)
{ write_actor_target (json, value); }
inline void from_json (const nlohmann::json &json, actor_bind_req_t &value)
{ read_actor_target (json, value); }
inline void to_json (nlohmann::json &json, const actor_bind_res_t &value)
{ write_actor_target (json, value); }
inline void from_json (const nlohmann::json &json, actor_bind_res_t &value)
{ read_actor_target (json, value); }

inline void to_json (nlohmann::json &json, const actor_relay_req_t &value)
{
    json = nlohmann::json{{"actorId", value.actor_id}, {"message", value.message}};
}

inline void from_json (const nlohmann::json &json, actor_relay_req_t &value)
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

    bool wait_for (std::chrono::milliseconds timeout)
    {
        std::unique_lock lock (_mutex);
        return _changed.wait_for (lock, timeout, [&] { return _open; });
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

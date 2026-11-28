/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/framework/contracts/configuration/logging.hpp>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>

class sealed_join_observation_t
{
  public:
    sealed_join_observation_t (std::string actor,
                               std::string flow_path,
                               std::function<void (const std::string &)> append) :
        _actor (std::move (actor)), _flow_path (std::move (flow_path)), _append (std::move (append))
    {
        const auto *enabled = std::getenv ("ZLINK_CROSS_FAIL_JOIN_COMPLETION");
        _enabled = enabled && std::string (enabled) == "1";
        if (_enabled) {
            const auto *sender = std::getenv ("ZLINK_CROSS_LATE_SENDER_RID");
            if (!sender || !*sender)
                throw std::runtime_error ("Missing ZLINK_CROSS_LATE_SENDER_RID");
            _sender = sender;
        }
    }

    bool enabled () const { return _enabled; }

    void fail_completion (const std::string &actor, const std::string &node)
    {
        if (!_enabled || actor != _actor)
            return;
        _append ("late-actor-completion-throw|actor=" + actor + "|nodeRid=" + node);
        _append ("late-actor-owner-resumed|nodeRid=" + node);
        throw std::runtime_error ("cross-language Join completion failure");
    }

    void observe (const zlink::framework::log_record_t &record)
    {
        std::map<std::string, std::string> fields;
        for (const auto &field : record.fields)
            fields[field.key] = field.value;
        const auto event = fields["event_id"];
        if (event != "zlink.message_flow" && event != "zlink.dispatch_error")
            return;
        if (event == "zlink.dispatch_error" && _enabled) {
            const auto source = fields.find ("source_rid");
            if (source != fields.end ()
                && source->second == zlink::routing_id_t::from (_sender).to_hex ())
                source->second = _sender;
        }
        std::string line = " event=" + event;
        for (const auto &[key, value] : fields)
            if (key != "event_id")
                line += " " + key + "=" + value;
        {
            const std::lock_guard<std::mutex> lock (_mutex);
            std::ofstream file (_flow_path, std::ios::app);
            file << line << " \n";
        }
        if (event != "zlink.dispatch_error" || fields["source_rid"] != _sender
            || fields["actor"] != _actor)
            return;
        const auto &kind = fields["kind"];
        if (kind == "send" || kind == "request")
            _append ("late-actor-owner-ingress|kind="
                     + std::string (kind == "send" ? "actorSend" : "actorRequest")
                     + "|source=" + _sender + "|actor=" + _actor);
    }

  private:
    std::string _actor, _flow_path, _sender;
    std::function<void (const std::string &)> _append;
    std::mutex _mutex;
    bool _enabled = false;
};

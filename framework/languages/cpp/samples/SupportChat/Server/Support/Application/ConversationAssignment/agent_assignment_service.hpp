/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace zlink::samples::supportchat
{

struct available_agent_t
{
    std::string roster_actor_id;
    std::string display_name;
};

class agent_availability_directory_t
{
  public:
    explicit agent_availability_directory_t (int capacity) : _capacity (capacity) {}

    void set_available (const std::string &actor_id,
                        std::string display_name,
                        bool available,
                        int active_conversations)
    {
        if (!available) {
            _agents.erase (actor_id);
            _order.erase (std::remove (_order.begin (), _order.end (), actor_id), _order.end ());
            return;
        }
        if (_agents.find (actor_id) == _agents.end ()) {
            _order.push_back (actor_id);
        }
        _agents[actor_id] = agent_slot_t{std::move (display_name), active_conversations};
    }

    std::optional<available_agent_t> assign ()
    {
        const auto available = std::find_if (_order.begin (), _order.end (), [this] (const auto &id) {
            return _agents.find (id)->second.active_conversations < _capacity;
        });
        if (available == _order.end ()) {
            return std::nullopt;
        }
        const auto actor_id = *available;
        auto &slot = _agents.find (actor_id)->second;
        ++slot.active_conversations;
        _order.erase (available);
        _order.push_back (actor_id);
        return available_agent_t{actor_id, slot.display_name};
    }

    void release (const std::string &actor_id)
    {
        const auto current = _agents.find (actor_id);
        if (current != _agents.end () && current->second.active_conversations > 0) {
            --current->second.active_conversations;
        }
    }

  private:
    struct agent_slot_t
    {
        std::string display_name;
        int active_conversations{0};
    };

    int _capacity;
    std::unordered_map<std::string, agent_slot_t> _agents;
    std::vector<std::string> _order;
};

class agent_assignment_service_t
{
  public:
    explicit agent_assignment_service_t (agent_availability_directory_t &directory) :
        _directory (directory)
    {
    }

    void set_available (const std::string &actor_id,
                        std::string display_name,
                        bool available)
    {
        int active_conversations = 0;
        for (const auto &[_, reserved_actor_id] : _reservations) {
            if (reserved_actor_id == actor_id) {
                ++active_conversations;
            }
        }
        _directory.set_available (actor_id, std::move (display_name), available,
                                  active_conversations);
    }

    std::optional<available_agent_t> assign_for_conversation (const std::string &conversation_id)
    {
        if (_reservations.find (conversation_id) != _reservations.end ()) {
            return std::nullopt;
        }
        auto agent = _directory.assign ();
        if (agent) {
            _reservations[conversation_id] = agent->roster_actor_id;
        }
        return agent;
    }

    void release_conversation (const std::string &conversation_id)
    {
        const auto reserved = _reservations.find (conversation_id);
        if (reserved == _reservations.end ()) {
            return;
        }
        _directory.release (reserved->second);
        _reservations.erase (reserved);
    }

  private:
    agent_availability_directory_t &_directory;
    std::unordered_map<std::string, std::string> _reservations;
};

} // namespace zlink::samples::supportchat

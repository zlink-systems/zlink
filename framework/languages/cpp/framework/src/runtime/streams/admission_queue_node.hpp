/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "runtime/streams/admission_queue_messages.hpp"
#include "runtime/streams/admission_queue_setup.hpp"

#include <zlink/framework/contracts/channels/channel.hpp>
#include <zlink/framework/contracts/dispatch/task.hpp>

#include <functional>
#include <map>
#include <mutex>
#include <set>

namespace zlink::framework::detail
{
struct app_test_access_t;
class mesh_node_runtime_t;
}

namespace zlink::framework::runtime
{
class location_runtime_t;

namespace admission_queue
{

class queue_node_t : public std::enable_shared_from_this<queue_node_t>
{
  public:
    struct ports_t
    {
        std::function<std::optional<node_t> ()> local;
        std::function<std::optional<owner_t> ()> owner;
        std::function<task_t<json_t> (const char *, json_t)> request;
        std::function<task_t<void> (const release_t &)> release;
    };

    queue_node_t (settings_t settings, ports_t ports);
    queue_node_t (settings_t settings,
                  route_client_t routes,
                  std::shared_ptr<detail::mesh_node_runtime_t> mesh,
                  location_runtime_t &locations);

    std::optional<connection_t> connection (std::string_view stream,
                                            const zlink::routing_id_t &rid) const;
    std::uint64_t verify (const connection_t &identity, std::string_view ticket) const;
    bool requires_permit () const { return _settings.global_limit.has_value (); }
    task_t<std::shared_ptr<void>> admit (connection_t connection, std::uint64_t epoch);
    task_t<delivery_value_t> enqueue (connection_t connection);
    task_t<std::vector<delivery_t>> report (std::uint32_t limit, std::uint32_t in_flight);
    void used (const connection_t &connection);
    void ended (const connection_t &connection);

  private:
    friend struct detail::app_test_access_t;

    void release (const node_t &node, std::uint64_t epoch, std::uint64_t id) noexcept;
    void clear_epoch ();

    settings_t _settings;
    ports_t _ports;
    mutable std::mutex _mutex;
    std::uint64_t _epoch{};
    std::uint64_t _last_seq{};
    std::set<std::uint64_t> _pending;
    std::map<std::uint64_t, std::uint64_t> _held;
    std::set<session_t> _used;
    std::set<session_t> _ended;
};

} // namespace admission_queue
} // namespace zlink::framework::runtime

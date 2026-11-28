/* SPDX-License-Identifier: Elastic-2.0 */
#include "runtime/streams/admission_queue_node.hpp"

#include "runtime/locations/location_runtime.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/streams/admission_queue_state.hpp"

namespace zlink::framework::detail
{
struct route_client_access_t
{
    static auto
    send (route_client_t &client, spot_id_t spot, const char *packet, nlohmann::json payload)
    {
        return client.send_to_spot_named (std::move (spot), std::move (payload), packet);
    }

    static auto
    request (route_client_t &client, spot_id_t spot, const char *packet, nlohmann::json payload)
    {
        return client.request_to_spot_named (std::move (spot), std::move (payload), packet);
    }
};
}

namespace zlink::framework::runtime::admission_queue
{
namespace
{
bytes_t rid_bytes (const zlink::routing_id_t &rid)
{
    const auto raw = rid.to_bytes ();
    const auto bytes = std::as_bytes (std::span (raw));
    return {bytes.begin (), bytes.end ()};
}

[[noreturn]] void queue_unavailable ()
{
    throw framework_exception_t (framework_error_kind_t::unavailable,
                                 "Authentication admission queue is unavailable");
}
}

namespace
{
queue_node_t::ports_t node_ports (const settings_t &settings,
                                  route_client_t routes,
                                  std::shared_ptr<detail::mesh_node_runtime_t> mesh,
                                  location_runtime_t &locations)
{
    const auto &name = settings.mesh_name;
    const auto spot = spot_prefix + name;
    return {
      [mesh = std::move (mesh)] () -> std::optional<node_t> {
          const auto status = mesh->native_node ().status ();
          if (!status.lifecycle_generation ())
              return {};
          return node_t{rid_bytes (status.routing_id ()), status.lifecycle_generation ()};
      },
      [&locations] () -> std::optional<owner_t> {
          const auto owner = locations.current_owner_token ();
          if (!owner)
              return {};
          return owner_t{owner->owner_id, static_cast<std::uint64_t> (owner->lease_generation)};
      },
      [routes, spot, name] (const char *packet, json_t payload) mutable -> task_t<json_t> {
          return detail::route_client_access_t::request (routes, spot, packet, std::move (payload))
            .instance_spot (spot_type)
            .in_mesh (name)
            .async<json_t> ();
      },
      [routes, spot] (const release_t &release) mutable {
          return detail::route_client_access_t::send (routes, spot, release_packet,
                                                      write_admission_message (release))
            .async ();
      }};
}
} // namespace

queue_node_t::queue_node_t (settings_t settings, ports_t ports) :
    _settings (std::move (settings)), _ports (std::move (ports))
{
}

queue_node_t::queue_node_t (settings_t settings,
                            route_client_t routes,
                            std::shared_ptr<detail::mesh_node_runtime_t> mesh,
                            location_runtime_t &locations) :
    queue_node_t (settings, node_ports (settings, std::move (routes), std::move (mesh), locations))
{
}

std::optional<connection_t> queue_node_t::connection (std::string_view stream,
                                                      const zlink::routing_id_t &rid) const
{
    const auto node = _ports.local ();
    if (!node)
        return {};
    return connection_of (*node, {std::string (stream), rid_bytes (rid)});
}

std::uint64_t queue_node_t::verify (const connection_t &identity, std::string_view ticket) const
{
    std::uint64_t epoch;
    {
        const std::lock_guard lock (_mutex);
        epoch = _epoch;
    }
    return _settings.tickets->verify (identity, ticket, epoch, queue_time_t::now ().unix_ms) ? epoch
                                                                                             : 0;
}

task_t<std::shared_ptr<void>> queue_node_t::admit (connection_t connection, std::uint64_t epoch)
{
    const auto owner = _ports.owner ();
    if (!owner)
        queue_unavailable ();
    std::uint64_t seq;
    {
        const std::lock_guard lock (_mutex);
        if (epoch != _epoch)
            co_return std::shared_ptr<void>{};
        if (_last_seq == std::numeric_limits<std::uint64_t>::max ())
            queue_unavailable ();
        seq = ++_last_seq;
        _pending.insert (seq);
    }
    std::uint64_t id;
    try {
        admit_request_t admit{connection, *owner, epoch, seq};
        id = read_admit_reply (
          co_await _ports.request (admit_packet, write_admission_message (admit)));
    }
    catch (const std::exception &) {
        const std::lock_guard lock (_mutex);
        _pending.erase (seq);
        throw;
    }
    {
        const std::lock_guard lock (_mutex);
        if (id && _epoch == epoch)
            _held.emplace (id, epoch);
        else
            id = 0;
        _pending.erase (seq);
    }
    if (!id)
        co_return std::shared_ptr<void>{};
    auto self = shared_from_this ();
    co_return std::shared_ptr<void> (this,
                                     [self = std::move (self), node = node_of (connection), epoch,
                                      id] (void *) { self->release (node, epoch, id); });
}

void queue_node_t::release (const node_t &node, std::uint64_t epoch, std::uint64_t id) noexcept
{
    {
        const std::lock_guard lock (_mutex);
        const auto permit = _held.find (id);
        if (permit == _held.end () || permit->second != epoch)
            return;
        _held.erase (permit);
    }
    try {
        auto task = _ports.release (release_t{node, id});
        // The resolved prefix and held list allow the next report to reclaim failed sends.
        (void) task;
    }
    catch (const std::exception &) {
        // The permit has already been removed from the next report.
    }
}

task_t<delivery_value_t> queue_node_t::enqueue (connection_t connection)
{
    try {
        auto reply =
          read_enqueue_reply (co_await _ports.request (enqueue_packet, write_enqueue (connection)));
        if (reply.delivery.session != session_of (connection))
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "Admission enqueue response has the wrong session");
        co_return std::move (reply.delivery.value);
    }
    catch (const std::exception &) {
        clear_epoch ();
        throw;
    }
}

task_t<std::vector<delivery_t>> queue_node_t::report (std::uint32_t limit, std::uint32_t in_flight)
{
    const auto identity = _ports.local ();
    if (!identity) {
        clear_epoch ();
        co_return std::vector<delivery_t>{};
    }
    std::set<session_t> used;
    std::set<session_t> ended;
    node_report_t report{.node = *identity,
                         .limit = static_cast<std::int32_t> (limit),
                         .in_flight = static_cast<std::int32_t> (in_flight)};
    {
        const std::lock_guard lock (_mutex);
        used.swap (_used);
        ended.swap (_ended);
        report.resolved_request_seq = _pending.empty () ? _last_seq : *_pending.begin () - 1;
        for (const auto &[id, epoch] : _held)
            report.held.push_back (id);
    }
    try {
        report.used.assign (used.begin (), used.end ());
        report.ended.assign (ended.begin (), ended.end ());
        auto reply = read_report_reply (
          co_await _ports.request (report_packet, write_admission_message (report)));
        {
            const std::lock_guard lock (_mutex);
            if (reply.epoch != _epoch) {
                std::erase_if (_held,
                               [&] (const auto &permit) { return permit.second != reply.epoch; });
                _epoch = reply.epoch;
            }
        }
        co_return std::move (reply.deliveries);
    }
    catch (const std::exception &) {
        const std::lock_guard lock (_mutex);
        _epoch = 0;
        _used.merge (used);
        _ended.merge (ended);
        throw;
    }
}

void queue_node_t::used (const connection_t &connection)
{
    const std::lock_guard lock (_mutex);
    _used.emplace (session_of (connection));
}

void queue_node_t::ended (const connection_t &connection)
{
    const std::lock_guard lock (_mutex);
    _ended.emplace (session_of (connection));
}

void queue_node_t::clear_epoch ()
{
    const std::lock_guard lock (_mutex);
    _epoch = 0;
}

} // namespace zlink::framework::runtime::admission_queue

/* SPDX-License-Identifier: Elastic-2.0 */

#include "actor_maintenance_join_stream_fixture.hpp"

namespace zlink::framework::test::actor_maintenance_join
{
relay_session_t::relay_session_t (fw::actor_manager_t &actors,
                                  std::shared_ptr<observations_t> evidence) :
    _actors (actors), observations (std::move (evidence))
{
}

fw::task_t<void> relay_session_t::on_connected (fw::stream_t &)
{
    co_return;
}

fw::task_t<void> relay_session_t::on_disconnected (fw::stream_t &)
{
    co_return;
}

fw::task_t<void> relay_session_t::on_error (fw::stream_t &, const fw::stream_error_t &)
{
    co_return;
}

fw::task_t<void> relay_session_t::on_packet (fw::stream_t &stream,
                                             const fw::session_message_context_t &dispatch,
                                             const zlink::framework::message_t &payload)
{
    auto bound = stream.actors ().bound ();
    if (bound.empty ()) {
        const auto ensured = co_await _actors.get_or_create (fw::actor_id_t (actor_id), actor_type)
                               .in_mesh (mesh_name)
                               .async ();
        const auto ref = std::visit (
          [] (const auto &result) -> fw::actor_ref_t {
              if constexpr (requires { result.actor; })
                  return result.actor;
              else
                  throw std::runtime_error ("Actor creation rejected");
          },
          ensured);
        co_await stream.actors ().bind_or_get (ref).async ();
        bound = stream.actors ().bound ();
    }
    co_await bound.front ().relay (std::string (dispatch.packet_name), payload);
    {
        std::lock_guard lock (observations->mutex);
        observations->relay_completed = true;
    }
    observations->changed.notify_all ();
}

view_client_t::view_client_t (std::string endpoint)
{
    zlink::stream_connector::connector_options_t options;
    options.endpoint = std::move (endpoint);
    options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
    options.reconnect.enabled = false;
    _connector = zlink::stream_connector::connector_factory_t::create (options);
    _closed_subscription = _connector.on_disconnected ([this] (auto) {
        {
            std::lock_guard lock (_mutex);
            _closed = true;
        }
        _changed.notify_all ();
    });
    _subscription = _connector.on<view_t> ([this] (const auto &message) {
        {
            std::lock_guard lock (_mutex);
            _views.push_back (message.payload.instance);
        }
        _changed.notify_all ();
    });
}

view_client_t::~view_client_t ()
{
    (void) _connector.close ();
}

bool view_client_t::connect ()
{
    return static_cast<bool> (_connector.connect ());
}

bool view_client_t::wait_closed ()
{
    std::unique_lock lock (_mutex);
    return _changed.wait_for (lock, wait_budget, [&] { return _closed; });
}

std::optional<int> view_client_t::wait_view (std::size_t index)
{
    std::unique_lock lock (_mutex);
    if (!_changed.wait_for (lock, wait_budget, [&] { return _views.size () > index; }))
        return std::nullopt;
    return _views[index];
}

std::size_t view_client_t::received_views ()
{
    std::lock_guard lock (_mutex);
    return _views.size ();
}

std::optional<int> view_client_t::view ()
{
    std::unique_lock lock (_mutex);
    const auto before = _views.size ();
    lock.unlock ();
    _connector.send (view_request_t{}).submit ();
    lock.lock ();
    if (!_changed.wait_for (lock, wait_budget, [&] { return _views.size () > before; }))
        return std::nullopt;
    return _views[before];
}

template void configure_session<relay_session_t> (host_t &,
                                                  const std::shared_ptr<observations_t> &,
                                                  std::shared_ptr<fw::location_store_t>,
                                                  std::shared_ptr<fw::relocation_store_t>);
} // namespace zlink::framework::test::actor_maintenance_join

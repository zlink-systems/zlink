/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <zlink/framework.hpp>
#include "runtime/locations/in_memory_store_providers.hpp"
#include <iostream>
#include <memory>
#include <sstream>
#include <type_traits>
namespace
{
struct startup_dependency_t
{
    static inline int constructed = 0;
    startup_dependency_t () { ++constructed; }
};

class startup_spot_t final : public zlink::framework::spot_t<zlink::framework::actor_t>
{
  public:
    static inline int constructed = 0;
    startup_spot_t (zlink::framework::spot_context_t context,
                    startup_dependency_t &,
                    zlink::framework::actor_client_t &,
                    zlink::framework::actor_manager_t &,
                    zlink::framework::spot_manager_t &,
                    zlink::framework::spot_publisher_client_t &,
                    zlink::framework::route_mesh_runtime_t &,
                    zlink::framework::route_mesh_runtime_options_t &,
                    zlink::framework::client_server_runtime_t &) :
        _context (std::move (context))
    {
        ++constructed;
    }
    zlink::framework::spot_context_t &context () noexcept override { return _context; }
    const zlink::framework::spot_context_t &context () const noexcept override { return _context; }
    void configure () override {}
    zlink::framework::task_t<zlink::framework::spot_actor_join_result_t>
    on_actor_join (std::string_view, const zlink::framework::message_t &) override
    {
        co_return zlink::framework::spot_actor_join_result_t::reject ();
    }
    zlink::framework::task_t<void> on_actor_joined (zlink::framework::actor_t &) override
    {
        co_return;
    }
    zlink::framework::task_t<void> on_leave_actor (zlink::framework::actor_t &) override
    {
        co_return;
    }

  private:
    zlink::framework::spot_context_t _context;
};

class stop_after_start_t final : public zlink::framework::hosted_service_t
{
  public:
    explicit stop_after_start_t (zlink::framework::app_t &app) : _app (app) {}
    zlink::framework::task_t<void> start (zlink::framework::service_provider_t &) override
    {
        _app.stop ();
        co_return;
    }
    void stop () noexcept override {}

  private:
    zlink::framework::app_t &_app;
};

template <bool ActorHandler>
class owner_entry_t : public zlink::framework::entry_spot_t<zlink::framework::actor_t>
{
  public:
    explicit owner_entry_t (zlink::framework::entry_spot_context_t context) :
        _context (std::move (context))
    {
    }
    zlink::framework::entry_spot_context_t &context () noexcept override { return _context; }
    const zlink::framework::entry_spot_context_t &context () const noexcept override
    {
        return _context;
    }
    void configure () override
    {
        if constexpr (ActorHandler)
            _context.handlers ().template add_actor_send<&owner_entry_t::on_actor_message> (
              "owner");
        else
            _context.handlers ().template add_handler<&owner_entry_t::on_message> ("owner");
    }
    void on_message (const std::string &) {}
    void on_actor_message (zlink::framework::actor_t &,
                           const zlink::framework::message_context_t &,
                           const std::string &)
    {
    }
    zlink::framework::task_t<void> on_actor_joined (zlink::framework::actor_t &) override
    {
        co_return;
    }
    zlink::framework::task_t<void> on_leave_actor (zlink::framework::actor_t &) override
    {
        co_return;
    }

  private:
    zlink::framework::entry_spot_context_t _context;
};

template <bool ActorHandler> class derived_owner_entry_t : public owner_entry_t<ActorHandler>
{
  public:
    using owner_entry_t<ActorHandler>::owner_entry_t;
};

static_assert (std::is_same_v<decltype (&derived_owner_entry_t<false>::on_message),
                              decltype (&owner_entry_t<false>::on_message)>);

template <typename TEntry, typename TOwner> bool verify_handler_owner (bool exact)
{
    using namespace zlink::framework;
    auto app = app_t::create ();
    auto &options = app.add_zlink_framework ();
    options.add_location_store (std::make_shared<runtime::in_memory_location_store_t> ());
    options.add_route_mesh ("owner")
      .listen ("tcp://127.0.0.1:0")
      .set_routing_id (zlink::routing_id_t::from ("owner"))
      .objects ()
      .server ()
      .template add_entry_spot<TEntry> ();
    app.add_hosted_service (std::make_unique<stop_after_start_t> (app));
    std::ostringstream errors;
    auto *previous = std::cerr.rdbuf (errors.rdbuf ());
    const auto result = app.run (0, nullptr);
    std::cerr.rdbuf (previous);
    const auto message = errors.str ();
    if (exact) {
        if (result != 0)
            std::cerr << "exact handler owner startup failed: " << message << '\n';
        return result == 0;
    }
    const bool rejected = result != 0 && message.find ("SPOT handler") != std::string::npos
                          && message.find (typeid (TOwner).name ()) != std::string::npos
                          && message.find (typeid (TEntry).name ()) != std::string::npos;
    if (!rejected)
        std::cerr << "handler owner contract failed: " << message << '\n';
    return rejected;
}

bool verify_spot_startup_metadata (bool supply_dependency)
{
    using namespace zlink::framework;
    auto app = app_t::create ();
    if (supply_dependency)
        app.advanced ().services ().add_scoped<startup_dependency_t> ();
    auto &options = app.add_zlink_framework ();
    options.configure_locations ().owner_lease_renew_interval = std::chrono::milliseconds (10);
    options.configure_locations ().owner_lease_fencing_margin = std::chrono::milliseconds (1);
    options.add_location_store (std::make_shared<runtime::in_memory_location_store_t> ());
    options.add_route_mesh ("metadata")
      .listen ("tcp://127.0.0.1:0")
      .set_routing_id (zlink::routing_id_t::from ("metadata"))
      .objects ()
      .server ()
      .add_spot_factory<startup_spot_t, startup_dependency_t, actor_client_t, actor_manager_t,
                        spot_manager_t, spot_publisher_client_t, route_mesh_runtime_t,
                        route_mesh_runtime_options_t, client_server_runtime_t> ("metadata")
      .disable_relocation ();
    app.add_hosted_service (std::make_unique<stop_after_start_t> (app));
    const auto result = app.run (0, nullptr);
    if ((supply_dependency ? result != 0 : result == 0) || startup_dependency_t::constructed != 0
        || startup_spot_t::constructed != 0)
        std::cerr << "startup metadata: supplied=" << supply_dependency << " result=" << result
                  << " dependency constructors=" << startup_dependency_t::constructed
                  << " spot constructors=" << startup_spot_t::constructed << '\n';
    return (supply_dependency ? result == 0 : result != 0) && startup_dependency_t::constructed == 0
           && startup_spot_t::constructed == 0;
}

}
int main ()
{
    return verify_spot_startup_metadata (false) && verify_spot_startup_metadata (true)
               && verify_handler_owner<owner_entry_t<false>, owner_entry_t<false>> (true)
               && verify_handler_owner<derived_owner_entry_t<false>, owner_entry_t<false>> (false)
               && verify_handler_owner<owner_entry_t<true>, owner_entry_t<true>> (true)
               && verify_handler_owner<derived_owner_entry_t<true>, owner_entry_t<true>> (false)
             ? 0
             : 1;
}

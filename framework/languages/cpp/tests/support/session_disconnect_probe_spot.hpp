/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include "runtime/spots/spot_runtime.hpp"

#include <functional>
#include <memory>
#include <string_view>
#include <utility>

namespace zlink::framework::test
{

class session_disconnect_probe_spot_t
{
  public:
    using actor_type = int;
    using disconnect_t = std::function<task_t<void> (void *, void *)>;

    explicit session_disconnect_probe_spot_t (disconnect_t disconnect) :
        _disconnect (std::move (disconnect))
    {
    }

    task_t<spot_actor_join_result_t> on_actor_join (std::string_view, const message_t &)
    {
        co_return spot_actor_join_result_t::accept ();
    }
    task_t<void> on_actor_joined (int &) { co_return; }
    task_t<void> on_leave_actor (int &) { co_return; }
    task_t<void> on_disconnect_actor (int &actor) { return _disconnect (this, &actor); }
    task_t<void> on_probe (int &, message_context_t &, const int &) { co_return; }

  private:
    disconnect_t _disconnect;
};

inline void
register_session_disconnect_probe (const std::shared_ptr<detail::spot_context_state_t> &spot,
                                   session_disconnect_probe_spot_t::disconnect_t disconnect)
{
    spot->spot_instance =
      std::make_shared<session_disconnect_probe_spot_t> (std::move (disconnect));
    detail::spot_context_access_t::create (spot)
      .handlers ()
      .add_actor_send<&session_disconnect_probe_spot_t::on_probe> (
        "session-disconnect-probe-register");
}

} // namespace zlink::framework::test

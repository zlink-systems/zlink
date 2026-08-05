/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework.hpp>

#include <stdexcept>

namespace zlink::framework::e2e::automatic_turn_dispatch::server
{

template <typename T>
T read_role_options (app_t &app, int argc, char **argv, const char *role)
{
    app.config ().load_cli (argc, argv);
    const auto path = app.config ().model ().get ("config");
    if (!path) {
        throw std::runtime_error (std::string ("AutomaticTurnDispatch ") + role
                                  + " requires --config=<path>");
    }
    app.config ().load_json (*path);
    return app.config ().bind_required<T> ("e2e");
}

} // namespace zlink::framework::e2e::automatic_turn_dispatch::server

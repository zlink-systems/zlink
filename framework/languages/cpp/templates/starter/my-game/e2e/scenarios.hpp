#pragma once
#include "client.hpp"

namespace MyGame::E2E
{
// Connects one public STREAM client, checks it stays healthy, and disconnects it, then
// drains the empty host and stops the node.
inline void connect (const std::string &endpoint, World &world)
{
    Client client (endpoint);
    client.healthy ();
    client.close ();
    auto &session = world.session ();
    session.command (Control::drain);
    session.wait_line (Control::requested, Limits::relocation);
    session.wait_line (Control::relocated, Limits::relocation);
    session.wait_line (Control::safe, Limits::safe);
    world.stop_session ();
}
}

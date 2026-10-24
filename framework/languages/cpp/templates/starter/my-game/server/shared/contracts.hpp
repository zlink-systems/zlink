#pragma once
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace MyGame
{
struct Names
{
    static constexpr auto mesh = "my-game-world";
    static constexpr auto stream = "players";
    // Node ID, role, config file stem and executable suffix of the Session node.
    static constexpr auto session = "session";
};
struct Limits
{
    static constexpr auto packet = std::chrono::seconds (10);
    static constexpr auto startup = std::chrono::seconds (30);
    static constexpr auto relocation = std::chrono::seconds (30);
    static constexpr auto message_follow = std::chrono::seconds (30);
    static constexpr auto safe = message_follow + relocation;
    static constexpr auto shutdown = std::chrono::seconds (30);
    static constexpr std::size_t observation_capacity = 128;
};
// The only definition of the stdin commands and stdout markers; the node and the runner share it.
struct Control
{
    static constexpr auto ready = "READY ";
    static constexpr auto drain = "drain";
    static constexpr auto shutdown = "shutdown";
    static constexpr auto requested = "RELOCATION_REQUESTED";
    static constexpr auto relocated = "RELOCATED";
    static constexpr auto safe = "SAFE_TO_SHUTDOWN";
    static constexpr auto stopped = "STOPPED";
};
}

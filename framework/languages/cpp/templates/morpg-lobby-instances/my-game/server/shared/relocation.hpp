#pragma once
#include "player.hpp"
namespace MyGame
{
// Only application state is serialized; Framework owns identity and membership.
template <class Spot, class State>
class StateRelocation final : public fx::spot_relocation_adapter_t<Spot>
{
  public:
    fx::task_t<std::vector<std::byte>> capture (Spot &spot, std::stop_token) override
    {
        const auto payload = zlink::message_t::from_json (spot.state);
        co_return std::vector<std::byte> (payload.bytes ().begin (), payload.bytes ().end ());
    }
    fx::task_t<void> restore (Spot &spot, std::vector<std::byte> payload, std::stop_token) override
    {
        spot.state = zlink::message_t::from (std::span<const std::byte> (payload))
                       .template parse_json<State> ();
        co_return;
    }
};
}

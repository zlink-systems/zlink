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
        const auto payload = fx::serializer_registry_t{}.get<State> ().serialize (spot.state);
        const auto bytes = payload.bytes ();
        co_return std::vector<std::byte> (bytes.begin (), bytes.end ());
    }
    fx::task_t<void> restore (Spot &spot, std::vector<std::byte> payload, std::stop_token) override
    {
        auto state = fx::serializer_registry_t{}.get<State> ().deserialize (
          fx::encoded_payload_t::from_bytes (std::span<const std::byte> (payload)));
        if constexpr (requires { state.after_relocation (); })
            state.after_relocation ();
        spot.state = std::move (state);
        co_return;
    }
};
}

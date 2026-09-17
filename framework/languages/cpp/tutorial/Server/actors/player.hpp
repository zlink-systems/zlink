#pragma once

#include "../../Shared/contracts.hpp"

#include <zlink/framework.hpp>

#include <memory>
#include <string>

namespace fw = zlink::framework;

// --8<-- [start:actor-class]
// A player is addressed by its own id and carries state that outlives any one
// connection. Like a room, its messages run one at a time.
struct player_t : fw::actor_t
{
    std::string nickname = "anonymous";

    fw::actor_context_t &context () noexcept override { return *_context; }
    const fw::actor_context_t &context () const noexcept override { return *_context; }

    void bind_context (fw::actor_context_t value)
    {
        _context = std::make_unique<fw::actor_context_t> (std::move (value));
    }

    void rename (std::string value) { nickname = std::move (value); }

  private:
    std::unique_ptr<fw::actor_context_t> _context;
};
// --8<-- [end:actor-class]

// --8<-- [start:actor-factory]
// The Framework creates players through this factory rather than by calling a
// constructor, so dependencies can be injected here.
class player_factory_t : public fw::actor_factory_t<player_t>
{
  public:
    fw::task_t<std::shared_ptr<player_t>> create (fw::actor_context_t context,
                                                  std::stop_token) override
    {
        auto player = std::make_shared<player_t> ();
        player->bind_context (std::move (context));
        co_return player;
    }
};
// --8<-- [end:actor-factory]

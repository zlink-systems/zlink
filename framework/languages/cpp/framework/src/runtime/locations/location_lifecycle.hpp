/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/location_key_codec.hpp"
#include "runtime/locations/legacy_location_rows.hpp"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace zlink::framework::runtime
{

class location_lifecycle_t
{
  public:
    using actor_deactivate_callback_t = std::function<void (const actor_location_t &)>;

    struct actor_claim_result_t
    {
        location_write_status_t status = location_write_status_t::ignored_stale;
        actor_location_t actor;
        std::uint64_t store_generation = 0;
        std::chrono::system_clock::time_point updated_at{};
    };

    struct spot_claim_result_t
    {
        location_write_status_t status = location_write_status_t::ignored_stale;
        spot_location_t spot;
        std::chrono::system_clock::time_point updated_at{};
    };

    explicit location_lifecycle_t (location_runtime_t &runtime) :
        _location_runtime (&runtime), _state (std::make_shared<state_t> ())
    {
    }

    ~location_lifecycle_t ()
    {
        std::lock_guard lock (_state->gate);
        _state->active = false;
        _state->actors.clear ();
        _state->spots.clear ();
    }

    location_lifecycle_t (const location_lifecycle_t &) = delete;
    location_lifecycle_t &operator= (const location_lifecycle_t &) = delete;

    actor_claim_result_t claim_actor (actor_location_t actor,
                                      actor_deactivate_callback_t deactivate = {},
                                      bool takeover = false)
    {
        (void) takeover;
        const auto now = std::chrono::system_clock::now ();
        if (!actor.actor_ref) {
            return {location_write_status_t::ignored_stale, std::move (actor), 0, now};
        }
        const auto generation = actor.actor_ref->object_generation ();

        {
            std::lock_guard lock (_state->gate);
            if (_state->active) {
                _state->actors[actor_key (actor)] =
                  actor_claim_t{actor, generation,
                                std::move (deactivate)};
            }
        }
        return {location_write_status_t::stored, std::move (actor), generation, now};
    }

    location_write_result_t update_actor_location (actor_location_t actor)
    {
        actor_claim_t tracked;
        const auto key = actor_key (actor);
        {
            std::lock_guard lock (_state->gate);
            const auto found = _state->actors.find (key);
            if (found == _state->actors.end ()) {
                return {location_write_status_t::ignored_stale, 0, {}};
            }
            tracked = found->second;
        }

        std::lock_guard lock (_state->gate);
        const auto found = _state->actors.find (key);
        if (found == _state->actors.end ())
            return {location_write_status_t::ignored_stale, 0, {}};
        found->second.actor = std::move (actor);
        if (!found->second.actor.actor_ref) {
            return {location_write_status_t::ignored_stale, 0, {}};
        }
        found->second.store_generation =
          found->second.actor.actor_ref->object_generation ();
        return {location_write_status_t::stored,
                static_cast<std::int64_t> (found->second.store_generation),
                std::chrono::system_clock::now ()};
    }

    bool owns_actor (actor_location_key_t key) const
    {
        std::lock_guard lock (_state->gate);
        return _state->actors.contains (actor_key (key));
    }

    std::optional<location_owner_token_t> current_owner_token () const
    {
        return _location_runtime != nullptr
                 ? _location_runtime->current_owner_token ()
                 : std::nullopt;
    }

    spot_claim_result_t claim_spot (spot_location_t spot)
    {
        const auto now = std::chrono::system_clock::now ();
        {
            std::lock_guard lock (_state->gate);
            if (_state->active) {
                _state->spots[spot_key (spot)] = spot;
            }
        }
        return {location_write_status_t::stored, std::move (spot), now};
    }

    location_write_result_t release_spot (spot_location_key_t key)
    {
        spot_location_t spot;
        {
            std::lock_guard lock (_state->gate);
            const auto found = _state->spots.find (spot_key (key));
            if (found == _state->spots.end ()) {
                return {location_write_status_t::ignored_stale, 0, {}};
            }
            spot = found->second;
        }

        std::lock_guard lock (_state->gate);
        _state->spots.erase (spot_key (spot));
        return {location_write_status_t::stored, spot.generation,
                std::chrono::system_clock::now ()};
    }

    location_write_result_t renew_actor (actor_location_key_t key)
    {
        actor_location_t actor;
        {
            std::lock_guard lock (_state->gate);
            const auto found = _state->actors.find (actor_key (key));
            if (found == _state->actors.end ()) {
                return {location_write_status_t::ignored_stale, 0, {}};
            }
            actor = found->second.actor;
        }

        if (!actor.actor_ref) {
            return {location_write_status_t::ignored_stale, 0, {}};
        }
        return {location_write_status_t::stored,
                static_cast<std::int64_t> (actor.actor_ref->object_generation ()),
                std::chrono::system_clock::now ()};
    }

    location_write_result_t release_actor (actor_location_key_t key)
    {
        actor_claim_t claim;
        {
            std::lock_guard lock (_state->gate);
            const auto found = _state->actors.find (actor_key (key));
            if (found == _state->actors.end ()) {
                return {location_write_status_t::ignored_stale, 0, {}};
            }
            claim = found->second;
            // Untracked before the write: a stale remove after another owner's
            // takeover is ignored by the store and must not fire deactivation.
            _state->actors.erase (found);
        }

        return {location_write_status_t::stored,
                static_cast<std::int64_t> (claim.store_generation),
                std::chrono::system_clock::now ()};
    }

    std::size_t tracked_actor_count () const
    {
        std::lock_guard lock (_state->gate);
        return _state->actors.size ();
    }

  private:
    struct actor_claim_t
    {
        actor_location_t actor;
        std::uint64_t store_generation = 0;
        actor_deactivate_callback_t deactivate;
    };

    struct state_t
    {
        mutable std::mutex gate;
        bool active = true;
        std::map<std::string, actor_claim_t> actors;
        std::map<std::string, spot_location_t> spots;
    };

    static std::string actor_key (const actor_location_t &actor)
    {
        return actor_key (actor_location_key_t{actor.mesh_name, actor.actor_id});
    }

    static std::string actor_key (const actor_location_key_t &key)
    {
        return location_key_codec_t::encode_actor_key (key);
    }

    static std::string spot_key (const spot_location_t &spot)
    {
        return spot_key (spot_location_key_t{spot.spot_id});
    }

    static std::string spot_key (const spot_location_key_t &key)
    {
        return location_key_codec_t::encode_spot_key (key);
    }

    static void deactivate_actor (state_t &state, const std::string &key)
    {
        actor_claim_t claim;
        {
            std::lock_guard lock (state.gate);
            if (!state.active) {
                return;
            }
            const auto found = state.actors.find (key);
            if (found == state.actors.end ()) {
                return;
            }
            claim = std::move (found->second);
            state.actors.erase (found);
        }
        if (claim.deactivate) {
            claim.deactivate (claim.actor);
        }
    }

    static void deactivate_all (state_t &state)
    {
        std::vector<actor_claim_t> claims;
        {
            std::lock_guard lock (state.gate);
            if (!state.active) {
                return;
            }
            for (auto &entry : state.actors) {
                claims.push_back (std::move (entry.second));
            }
            state.actors.clear ();
        }
        for (const auto &claim : claims) {
            if (claim.deactivate) {
                claim.deactivate (claim.actor);
            }
        }
    }

    location_runtime_t *_location_runtime = nullptr;
    std::shared_ptr<state_t> _state;
};

} // namespace zlink::framework::runtime

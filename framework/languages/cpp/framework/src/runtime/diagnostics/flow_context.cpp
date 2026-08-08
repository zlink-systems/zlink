/* SPDX-License-Identifier: MPL-2.0 */

#include "runtime/diagnostics/flow_context.hpp"
#include "runtime/execution/actor_execution_context.hpp"

#include <zlink/framework/contracts/dispatch/task.hpp>

#include <array>
#include <chrono>
#include <memory>
#include <random>

namespace zlink::framework::runtime
{

namespace
{

std::uint64_t random_u64 ()
{
    thread_local std::mt19937_64 engine{std::random_device{} ()};
    return engine ();
}

/* Coroutine-suspension flow propagation (flow-correlation MFLOW-EXT-014):
 * the task machinery captures the ambient flow when a continuation or
 * callback registers and re-enters it around the resume. The guard restores
 * the previous value, so the id never leaks into unrelated callbacks. */
struct ambient_context_snapshot_t
{
    std::optional<flow_value_t> flow;
    std::optional<actor_execution_context_t> actor;
};

class ambient_context_scope_t
{
  public:
    explicit ambient_context_scope_t (const ambient_context_snapshot_t &snapshot)
    {
        if (snapshot.flow)
            _flow.emplace (snapshot.flow);
        if (snapshot.actor)
            _actor.emplace (snapshot.actor->actor_key, snapshot.actor->spot_id);
    }

  private:
    std::optional<flow_context_t::scope_t> _flow;
    std::optional<actor_execution_scope_t> _actor;
};

std::shared_ptr<void> capture_ambient_flow ()
{
    const auto &current = flow_context_t::current ();
    const auto &actor = current_actor_execution;
    if (!current && actor.actor_key.empty () && actor.spot_id.empty ()) {
        return nullptr;
    }
    auto snapshot = std::make_shared<ambient_context_snapshot_t> ();
    if (current)
        snapshot->flow = *current;
    if (!actor.actor_key.empty () || !actor.spot_id.empty ())
        snapshot->actor = actor;
    return snapshot;
}

std::shared_ptr<void> enter_ambient_flow (const std::shared_ptr<void> &snapshot)
{
    auto *value = static_cast<ambient_context_snapshot_t *> (snapshot.get ());
    return std::make_shared<ambient_context_scope_t> (*value);
}

constexpr framework::detail::ambient_context_hooks_t ambient_flow_hooks{&capture_ambient_flow,
                                                                        &enter_ambient_flow};

const bool ambient_flow_hooks_installed = [] {
    framework::detail::ambient_context_hooks.store (&ambient_flow_hooks);
    return true;
}();

} // namespace

std::string flow_id_t::create ()
{
    std::array<std::uint8_t, 16> bytes{};
    auto high = random_u64 ();
    auto low = random_u64 ();
    for (int i = 0; i < 8; ++i) {
        bytes[static_cast<std::size_t> (i)] = static_cast<std::uint8_t> (high >> (56 - i * 8));
        bytes[static_cast<std::size_t> (8 + i)] = static_cast<std::uint8_t> (low >> (56 - i * 8));
    }
    const auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds> (
        std::chrono::system_clock::now ().time_since_epoch ())
        .count ();
    bytes[0] = static_cast<std::uint8_t> (milliseconds >> 40);
    bytes[1] = static_cast<std::uint8_t> (milliseconds >> 32);
    bytes[2] = static_cast<std::uint8_t> (milliseconds >> 24);
    bytes[3] = static_cast<std::uint8_t> (milliseconds >> 16);
    bytes[4] = static_cast<std::uint8_t> (milliseconds >> 8);
    bytes[5] = static_cast<std::uint8_t> (milliseconds);
    bytes[6] = static_cast<std::uint8_t> ((bytes[6] & 0x0F) | 0x70);
    bytes[8] = static_cast<std::uint8_t> ((bytes[8] & 0x3F) | 0x80);

    static constexpr char digits[] = "0123456789abcdef";
    std::string value;
    value.reserve (encoded_length);
    for (std::size_t i = 0; i < bytes.size (); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            value.push_back ('-');
        }
        value.push_back (digits[bytes[i] >> 4]);
        value.push_back (digits[bytes[i] & 0x0F]);
    }
    return value;
}

bool flow_id_t::is_valid (std::string_view value) noexcept
{
    if (value.size () != encoded_length) {
        return false;
    }
    for (std::size_t i = 0; i < value.size (); ++i) {
        const char c = value[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') {
                return false;
            }
            continue;
        }
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex) {
            return false;
        }
    }
    if (value[14] != '7') {
        return false;
    }
    const char variant = value[19];
    return variant == '8' || variant == '9' || variant == 'a' || variant == 'b';
}

} // namespace zlink::framework::runtime

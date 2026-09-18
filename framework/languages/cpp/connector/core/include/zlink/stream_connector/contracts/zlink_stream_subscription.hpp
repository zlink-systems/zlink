/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <cstdint>
#include <memory>
#include <utility>

namespace zlink::stream_connector
{

namespace detail
{
void remove_subscription (const std::shared_ptr<void> &state, std::uint64_t subscription_id);
}

/// Handle returned by every handler registration (stream-connector §7).
///
/// C++ states ownership with a value, so the handle owns the registration: the
/// destructor removes whatever is still registered. A caller keeps the handle
/// for as long as the handler must run, and a handler removed this way does not
/// run from any later dispatch. Removing the same registration twice is not an
/// error.
class subscription_t
{
  public:
    subscription_t () = default;

    ~subscription_t () { unsubscribe (); }

    subscription_t (subscription_t &&other) noexcept :
        _state (std::move (other._state)), _id (other._id)
    {
        other._state.reset ();
        other._id = 0;
    }

    subscription_t &operator= (subscription_t &&other) noexcept
    {
        if (this != &other) {
            unsubscribe ();
            _state = std::move (other._state);
            _id = other._id;
            other._state.reset ();
            other._id = 0;
        }
        return *this;
    }

    subscription_t (const subscription_t &) = delete;
    subscription_t &operator= (const subscription_t &) = delete;

    /// Removes the registration. Calling this again is not an error.
    void unsubscribe ()
    {
        if (_id == 0) {
            return;
        }
        const auto id = _id;
        _id = 0;
        if (auto state = _state.lock ()) {
            detail::remove_subscription (state, id);
        }
        _state.reset ();
    }

    /// Returns true while this handle still owns a live registration.
    bool active () const noexcept { return _id != 0 && !_state.expired (); }

  private:
    friend class connector_t;

    subscription_t (std::weak_ptr<void> state, std::uint64_t id) :
        _state (std::move (state)), _id (id)
    {
    }

    std::weak_ptr<void> _state;
    std::uint64_t _id = 0;
};

} // namespace zlink::stream_connector

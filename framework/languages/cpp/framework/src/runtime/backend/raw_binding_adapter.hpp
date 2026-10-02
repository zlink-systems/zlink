/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/backend/raw_route_port.hpp"

#include <zlink/Contracts/Messaging/message.hpp>

#include <utility>
#include <vector>

namespace zlink::framework::detail::backend
{

// A successful binding receive owns native parts until close(). This guard
// pairs that terminal release with the receive even when ownership transfer or
// metadata validation throws.
template <typename TReceived> class binding_received_release_t final
{
  public:
    explicit binding_received_release_t (TReceived &received) noexcept : _received (&received) {}

    binding_received_release_t (const binding_received_release_t &) = delete;
    binding_received_release_t &operator= (const binding_received_release_t &) = delete;

    ~binding_received_release_t () noexcept
    {
        try {
            _received->close ();
        }
        catch (...) {
        }
    }

  private:
    TReceived *_received;
};

// The C++ binding receive API retains its native message storage. This is the
// single binding-facing ownership boundary: it copies each part once into the
// Framework-owned representation before the binding receive envelope closes.
inline raw_message_t copy_binding_parts (const std::vector<zlink::message_t> &parts)
{
    raw_message_t result;
    result.reserve (parts.size ());
    for (const auto &part : parts) {
        result.push_back (part.to_bytes ());
    }
    return result;
}

inline std::vector<zlink::message_t>
copy_binding_messages (const std::vector<zlink::message_t> &parts)
{
    std::vector<zlink::message_t> result;
    result.reserve (parts.size ());
    for (const auto &part : parts) {
        result.push_back (part);
    }
    return result;
}

inline std::vector<zlink::message_t> materialize_binding_parts (raw_message_t parts)
{
    std::vector<zlink::message_t> result;
    result.reserve (parts.size ());
    for (auto &part : parts) {
        auto storage = std::make_unique<raw_bytes_t> (std::move (part));
        auto message = zlink::advanced::external_message_t::from (
          std::span<std::uint8_t> (*storage),
          [] (void *, void *hint) { delete static_cast<raw_bytes_t *> (hint); }, storage.get ());
        if (!message.valid ())
            throw std::bad_alloc ();
        storage.release ();
        result.push_back (std::move (message));
    }
    return result;
}

} // namespace zlink::framework::detail::backend

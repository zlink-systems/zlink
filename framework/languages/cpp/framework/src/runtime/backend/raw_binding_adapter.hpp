/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/backend/raw_route_port.hpp"

#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/Contracts/Messaging/request_result.hpp>

#include <cerrno>
#include <vector>

namespace zlink::framework::detail::backend
{

// A successful binding receive owns native parts until close(). This guard
// pairs that terminal release with the receive even when ownership transfer or
// metadata validation throws.
template <typename TReceived>
class binding_received_release_t final
{
  public:
    explicit binding_received_release_t (TReceived &received) noexcept :
        _received (&received)
    {
    }

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
inline raw_message_t copy_binding_parts (
  const std::vector<zlink::message_t> &parts)
{
    raw_message_t result;
    result.reserve (parts.size ());
    for (const auto &part : parts) {
        result.push_back (part.to_bytes ());
    }
    return result;
}

inline std::vector<zlink::message_t> copy_binding_messages (
  const std::vector<zlink::message_t> &parts)
{
    std::vector<zlink::message_t> result;
    result.reserve (parts.size ());
    for (const auto &part : parts) {
        result.push_back (part);
    }
    return result;
}

inline std::vector<zlink::message_t> materialize_binding_parts (
  const raw_message_t &parts)
{
    std::vector<zlink::message_t> result;
    result.reserve (parts.size ());
    for (const auto &part : parts) {
        result.push_back (zlink::message_t::from (part));
    }
    return result;
}

inline raw_request_result_t map_binding_request_result (
  zlink::request_result_t result) noexcept
{
    switch (result) {
        case zlink::request_result_t::ok:
            return raw_request_result_t::ok;
        case zlink::request_result_t::timed_out:
            return raw_request_result_t::timed_out;
        case zlink::request_result_t::terminated:
            return raw_request_result_t::terminated;
        default:
            return raw_request_result_t::failed;
    }
}

// Core socket README "submit retry" owns this table. Only an initial local
// submit failure can be a transient route absence. In particular, ENOENT on a
// completion terminal means disconnect_rid retired an already-issued WRITABLE
// token and must not be replayed.
inline bool transient_route_failure (
  zlink::submit_result_t result,
  int error,
  raw_request_failure_phase_t phase) noexcept
{
    if (phase != raw_request_failure_phase_t::initial_admission)
        return false;
    if (result == zlink::submit_result_t::not_connected)
        return error == ENOTCONN || error == EHOSTUNREACH;
    return result == zlink::submit_result_t::not_admitted
           && error == ECONNREFUSED;
}

} // namespace zlink::framework::detail::backend

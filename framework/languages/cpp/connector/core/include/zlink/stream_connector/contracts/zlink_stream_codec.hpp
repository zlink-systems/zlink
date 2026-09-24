/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/stream_connector/contracts/zlink_stream_enums.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace zlink::stream_connector
{

/// Typed payload codec injected once when the connector is created
/// (stream-connector §5.4).
///
/// The same codec serves typed send, typed request and typed receive. There is
/// no per-message-type registration and no per-operation choice: §5.4 keeps the
/// injection point on the creation options only. A connector created without
/// one uses the JSON codec.
class typed_codec_t
{
  public:
    virtual ~typed_codec_t () = default;

    /// Wire codec number recorded in the packet header (§5.4 codec table).
    virtual codec_t codec_id () const noexcept = 0;

    /// Transforms a serialized typed payload into its wire form.
    virtual std::vector<std::uint8_t> encode (const std::vector<std::uint8_t> &payload) const = 0;

    /// Transforms a wire payload back into the serialized typed form.
    virtual std::vector<std::uint8_t> decode (const std::vector<std::uint8_t> &payload) const = 0;
};

/// Default typed codec: JSON, with the payload already in its wire form.
std::shared_ptr<const typed_codec_t> json_typed_codec ();

/// Packet-name resolver injected once when the connector is created
/// (stream-connector §5.4).
///
/// The resolver sees the payload type's simple name and returns the packet
/// name to put on the wire. A packet name declared on the payload type itself
/// outranks the resolver, and a name given to the call builder outranks both
/// (cpp stream-connector §3).
class packet_name_resolver_t
{
  public:
    virtual ~packet_name_resolver_t () = default;

    virtual std::string resolve (std::string_view type_name) const = 0;
};

} // namespace zlink::stream_connector

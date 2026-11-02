/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once
#include <variant>

#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/framework/contracts/codecs/serializer.hpp>
#include <zlink/framework/contracts/dispatch/execution.hpp>
#include <zlink/framework/contracts/errors/result.hpp>
#include <zlink/json_profile.hpp>

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <typeindex>
#include <utility>
#include <vector>

namespace zlink::framework::runtime::messaging
{

enum class message_kind_t
{
    request = 1,
    response = 2,
    command = 3,
    publish = 4,
    error = 5
};

struct envelope_header_t
{
    message_kind_t kind = message_kind_t::request;
    std::string channel_name;
    std::string message_name;
    std::string content_type = zlink::detail::json_profile::content_type;
    std::string correlation_id;
    std::optional<std::string> deadline;
    std::optional<std::string> topic;
    std::optional<std::string> error_code;
    std::optional<std::string> error_message;
    std::optional<std::string> source;
    std::map<std::string, std::string> metadata;
    /* Optional pair (flow-correlation §3.1): always together or absent.
     * Every route/actor/spot relay preserves both values as-is. */
    std::optional<std::string> flow_id;
    std::optional<flow_origin_t> flow_origin;
    bool can_correlate_reply () const noexcept;
};

class message_parts_t
{
  public:
    message_parts_t () = default;
    message_parts_t (const message_parts_t &other);
    message_parts_t &operator= (const message_parts_t &other);
    message_parts_t (message_parts_t &&) noexcept = default;
    message_parts_t &operator= (message_parts_t &&) noexcept = default;
    static message_parts_t borrow (const std::vector<zlink::message_t> &parts);
    static message_parts_t borrow (std::span<const std::span<const std::byte>> parts);
    std::span<const std::byte> part_bytes (std::size_t index) const;
    message_parts_t (zlink::message_t header, zlink::message_t body);
    explicit message_parts_t (std::vector<zlink::message_t> parts);

    std::size_t size () const noexcept;
    const zlink::message_t &operator[] (std::size_t index) const;
    const std::vector<zlink::message_t> &items () const;
    std::vector<zlink::message_t> take_items () &&;

  private:
    friend class envelope_codec_t;
    std::variant<std::vector<zlink::message_t>,
                 const std::vector<zlink::message_t> *,
                 std::span<const std::span<const std::byte>>>
      _parts;
};

class envelope_codec_t
{
  public:
    static constexpr const char *default_content_type = zlink::detail::json_profile::content_type;

    message_parts_t encode_raw_body_parts (const envelope_header_t &header,
                                           zlink::message_t body) const;
    message_parts_t encode_parts (const envelope_header_t &header,
                                  std::type_index body_type,
                                  const void *body,
                                  const serializer_registry_t &serializers) const;

    template <typename TBody>
    message_parts_t encode_parts (const envelope_header_t &header,
                                  const TBody &body,
                                  const serializer_registry_t &serializers) const
    {
        const auto serializer = serializers.get<TBody> ();
        auto encoded = detail::encoded_payload_to_raw (serializer.serialize (body));
        return message_parts_t (encode_header (header, serializer.content_type ()),
                                std::move (encoded));
    }

    zlink::message_t encode_header (const envelope_header_t &header) const;
    /* capture_flow gates the observation-only flow pair (flow-correlation §4):
     * at Off the decoder neither validates nor retains flowId/flowOrigin, so
     * malformed observation fields can never fail a frame on an Off node.
     * Callers at a flow processing point pass capture_enabled(); callers that
     * never consume flow pass false. */
    /* On rejection, the optional context retains readable base fields;
     * metadata stays empty. Invalid JSON has no rejection context. */
    result_t<envelope_header_t>
    decode_header (const zlink::message_t &message,
                   bool capture_flow = true,
                   std::optional<envelope_header_t> *rejection_context = nullptr) const;
    result_t<envelope_header_t>
    decode_header (const message_parts_t &parts,
                   bool capture_flow = true,
                   std::optional<envelope_header_t> *rejection_context = nullptr) const;
    result_t<envelope_header_t>
    decode_header (std::span<const std::byte> bytes,
                   bool capture_flow = true,
                   std::optional<envelope_header_t> *rejection_context = nullptr) const;
    result_t<encoded_payload_t> decode_body_view (const message_parts_t &parts) const;
    result_t<zlink::message_t> decode_body (const message_parts_t &parts) const;

  private:
    zlink::message_t encode_header (const envelope_header_t &header,
                                    const std::string &content_type) const;
};

} // namespace zlink::framework::runtime::messaging

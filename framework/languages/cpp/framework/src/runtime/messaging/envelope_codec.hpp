/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once
#include <variant>

#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/framework/contracts/codecs/serializer.hpp>
#include <zlink/framework/contracts/dispatch/execution.hpp>
#include <zlink/framework/contracts/errors/result.hpp>
#include <zlink/json_profile.hpp>

#include <array>
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

// Internal text either borrows the retained wire header or owns decoded/created text.
// Copies preserve that representation; public contexts materialize their string values.
class header_text_t
{
  public:
    header_text_t () = default;
    header_text_t (const char *text) : _text (std::string (text)) {}
    header_text_t (std::string text) : _text (std::move (text)) {}
    header_text_t (const header_text_t &other) : _text (other._text)
    {
        if (const auto *view = std::get_if<std::string_view> (&other._text))
            _text = std::string (*view);
    }
    header_text_t &operator= (const header_text_t &other)
    {
        if (this != &other) {
            if (const auto *view = std::get_if<std::string_view> (&other._text))
                assign (*view);
            else
                _text = other._text;
        }
        return *this;
    }
    header_text_t (header_text_t &&) noexcept = default;
    header_text_t &operator= (header_text_t &&) noexcept = default;
    header_text_t (std::string_view text) : _text (std::string (text)) {}
    header_text_t (std::array<char, 20> text) : _text (text) {}
    static header_text_t view (std::string_view text)
    {
        header_text_t result;
        result._text = text;
        return result;
    }
    operator std::string_view () const noexcept
    {
        return std::visit (
          [] (const auto &text) { return std::string_view (text.data (), text.size ()); }, _text);
    }
    operator std::string () const & { return std::string (std::string_view (*this)); }
    operator std::string () &&
    {
        if (auto *owned = std::get_if<std::string> (&_text))
            return std::move (*owned);
        return std::string (std::string_view (*this));
    }
    bool empty () const noexcept { return std::string_view (*this).empty (); }
    std::size_t size () const noexcept { return std::string_view (*this).size (); }
    const char *data () const noexcept { return std::string_view (*this).data (); }
    const char *begin () const noexcept { return data (); }
    const char *end () const noexcept { return data () + size (); }
    auto find (char value) const noexcept { return std::string_view (*this).find (value); }
    void clear () { _text = std::string{}; }
    void assign (std::string_view text)
    {
        if (auto *owned = std::get_if<std::string> (&_text))
            owned->assign (text.data (), text.size ());
        else
            _text = std::string (text);
    }
    void resize (std::size_t size) { _text = std::string (size, '\0'); }
    char &operator[] (std::size_t index) { return std::get<std::string> (_text)[index]; }
    friend bool operator== (const header_text_t &a, const header_text_t &b) noexcept
    {
        return std::string_view (a) == std::string_view (b);
    }
    template <typename T>
        requires std::is_convertible_v<const T &, std::string_view>
    friend bool operator== (const header_text_t &a, const T &b) noexcept
    {
        return std::string_view (a) == std::string_view (b);
    }
    friend std::string operator+ (std::string a, const header_text_t &b)
    {
        a.append (b.data (), b.size ());
        return a;
    }
    friend std::string operator+ (const char *a, const header_text_t &b)
    {
        return std::string (a) + b;
    }
    friend std::string operator+ (const header_text_t &a, const char *b)
    {
        return std::string (a) + b;
    }

  private:
    std::variant<std::string, std::string_view, std::array<char, 20>> _text;
};

// Header copies own their scalar text. Only a moved invocation retains the
// original frame; copying a header never clones a native message.
struct envelope_header_storage_t
{
    envelope_header_storage_t () = default;
    envelope_header_storage_t (const envelope_header_storage_t &) noexcept {}
    envelope_header_storage_t &operator= (const envelope_header_storage_t &other) noexcept
    {
        if (this != &other)
            message.reset ();
        return *this;
    }
    envelope_header_storage_t (envelope_header_storage_t &&) noexcept = default;
    envelope_header_storage_t &operator= (envelope_header_storage_t &&) noexcept = default;
    std::optional<zlink::message_t> message;
};

struct envelope_header_t
{
    message_kind_t kind = message_kind_t::request;
    header_text_t channel_name;
    header_text_t message_name;
    header_text_t content_type = zlink::detail::json_profile::content_type;
    header_text_t correlation_id;
    std::optional<header_text_t> deadline;
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
    envelope_header_storage_t retained_header;
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
    // The caller retains the input frame until it moves into the invocation.
    result_t<envelope_header_t>
    decode_header_view (const zlink::message_t &message,
                        bool capture_flow = true,
                        std::optional<envelope_header_t> *rejection_context = nullptr) const;
    result_t<envelope_header_t>
    decode_header_view (const message_parts_t &parts,
                        bool capture_flow = true,
                        std::optional<envelope_header_t> *rejection_context = nullptr) const;
    result_t<encoded_payload_t> decode_body_view (const message_parts_t &parts) const;
    result_t<zlink::message_t> decode_body (const message_parts_t &parts) const;

  private:
    friend class client_call_codec_t;
    zlink::message_t encode_header (const envelope_header_t &header,
                                    std::string_view content_type) const;
};

} // namespace zlink::framework::runtime::messaging

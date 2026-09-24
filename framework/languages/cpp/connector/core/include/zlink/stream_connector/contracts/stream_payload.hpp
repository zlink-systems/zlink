/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/stream_connector/contracts/result.hpp>
#include <zlink/stream_connector/contracts/zlink_stream_enums.hpp>
#include <zlink/json_profile.hpp>

#include <concepts>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace zlink::stream_connector::detail
{

/* A payload type names its packet with a static member (cpp
 * stream-connector §3). std::string_view is the widest form the member can
 * take: `const char *`, `std::string_view` and `std::string` all convert to
 * it, so a type that spells the member as a string_view is not silently
 * pushed onto the fallback path. */
template <typename T>
concept static_packet_name = requires {
    { T::packet_name } -> std::convertible_to<std::string_view>;
};

template <typename T>
concept protobuf_descriptor_name = requires {
    { std::string (T::descriptor ()->name ()) } -> std::same_as<std::string>;
};

/* Compiler-supplied spelling of T, used only as the input to the simple-name
 * extraction below. The raw spelling differs between compilers; the extracted
 * simple name does not. */
template <typename T> constexpr std::string_view decorated_type_name () noexcept
{
#if defined(_MSC_VER) && !defined(__clang__)
    return __FUNCSIG__;
#else
    return __PRETTY_FUNCTION__;
#endif
}

/* Simple name of T with namespace qualifiers, elaborated-type keywords and
 * template arguments removed (stream-connector §5: the default packet name is
 * the payload type's simple name, and a name that varies with the compiler is
 * never used as a packet name). typeid(T).name() is exactly such a name — it
 * is the Itanium mangling on GCC/Clang and a `struct ns::T` spelling on MSVC —
 * so a client and a server built with different compilers would not agree on
 * it. */
template <typename T> std::string simple_type_name ()
{
    constexpr std::string_view decorated = decorated_type_name<T> ();
    std::string_view name = decorated;

#if defined(_MSC_VER) && !defined(__clang__)
    const auto open = name.find ("decorated_type_name<");
    if (open != std::string_view::npos) {
        name.remove_prefix (open + std::string_view ("decorated_type_name<").size ());
        const auto close = name.rfind (">(");
        if (close != std::string_view::npos) {
            name = name.substr (0, close);
        }
    }
#else
    const auto open = name.find ("T = ");
    if (open != std::string_view::npos) {
        name.remove_prefix (open + 4);
        const auto close = name.find_first_of (";]");
        if (close != std::string_view::npos) {
            name = name.substr (0, close);
        }
    }
#endif

    for (const std::string_view keyword :
         {std::string_view ("struct "), std::string_view ("class "), std::string_view ("enum "),
          std::string_view ("union ")}) {
        while (name.substr (0, keyword.size ()) == keyword) {
            name.remove_prefix (keyword.size ());
        }
    }

    /* Drop template arguments before dropping namespace qualifiers so that a
     * qualifier inside the argument list cannot be mistaken for the type's
     * own qualifier. */
    const auto template_open = name.find ('<');
    if (template_open != std::string_view::npos) {
        name = name.substr (0, template_open);
    }
    const auto qualifier = name.rfind ("::");
    if (qualifier != std::string_view::npos) {
        name.remove_prefix (qualifier + 2);
    }

    while (!name.empty () && (name.front () == ' ' || name.front () == '\t')) {
        name.remove_prefix (1);
    }
    while (!name.empty () && (name.back () == ' ' || name.back () == '\t')) {
        name.remove_suffix (1);
    }
    if (name.empty ()) {
        return std::string (decorated);
    }
    return std::string (name);
}

/* Default packet name for TMessage, before connector_options_t::name_resolver
 * and before a caller-supplied builder name. */
template <typename T> std::string message_packet_name ()
{
    if constexpr (static_packet_name<T>) {
        return std::string (std::string_view (T::packet_name));
    } else if constexpr (protobuf_descriptor_name<T>) {
        return std::string (T::descriptor ()->name ());
    } else {
        return simple_type_name<T> ();
    }
}

/* True when TMessage carries its own packet name; that name outranks a
 * configured name resolver (cpp stream-connector §3). */
template <typename T> constexpr bool has_static_packet_name () noexcept
{
    return static_packet_name<T> || protobuf_descriptor_name<T>;
}

template <typename TMessage>
auto to_packet_payload (const TMessage &message, int) -> decltype (to_stream_payload (message),
                                                                   std::vector<std::uint8_t>{})
{
    return to_stream_payload (message);
}

template <typename TMessage>
    requires requires (const TMessage &message, std::string *bytes) {
        { message.SerializeToString (bytes) } -> std::same_as<bool>;
    }
std::vector<std::uint8_t> to_packet_payload (const TMessage &message, long)
{
    std::string bytes;
    if (!message.SerializeToString (&bytes)) {
#if ZLINK_STREAM_CONNECTOR_HAS_EXCEPTIONS
        throw std::runtime_error ("typed protobuf payload serialization failed");
#else
        bytes.clear ();
#endif
    }
    return {bytes.begin (), bytes.end ()};
}

template <typename TMessage>
std::vector<std::uint8_t> to_packet_payload (const TMessage &message, ...)
{
    const auto bytes = zlink::detail::json_profile::dump (nlohmann::json (message));
    return {bytes.begin (), bytes.end ()};
}

template <typename TMessage>
auto apply_packet_payload (TMessage &message,
                           const std::vector<std::uint8_t> &payload,
                           int) -> decltype (from_stream_payload (payload, message), void ());

template <typename TMessage>
void apply_packet_payload (TMessage &, const std::vector<std::uint8_t> &, ...);

template <typename TMessage>
    requires requires (TMessage &message, const std::string &bytes) {
        { message.ParseFromString (bytes) } -> std::same_as<bool>;
    }
void apply_packet_payload (TMessage &message, const std::vector<std::uint8_t> &payload, long)
{
    if (!message.ParseFromString (std::string (payload.begin (), payload.end ()))) {
#if ZLINK_STREAM_CONNECTOR_HAS_EXCEPTIONS
        throw std::runtime_error ("typed protobuf payload parse failed");
#endif
    }
}

template <typename TMessage>
auto apply_packet_payload (TMessage &message,
                           zlink::stream_connector::codec_t codec,
                           const std::vector<std::uint8_t> &payload,
                           int) -> decltype (from_stream_payload (codec, payload, message), void ())
{
    from_stream_payload (codec, payload, message);
}

template <typename TMessage>
void apply_packet_payload (TMessage &message,
                           zlink::stream_connector::codec_t,
                           const std::vector<std::uint8_t> &payload,
                           ...)
{
    apply_packet_payload (message, payload, 0);
}

template <typename TMessage>
auto apply_packet_payload (TMessage &message,
                           const std::vector<std::uint8_t> &payload,
                           int) -> decltype (from_stream_payload (payload, message), void ())
{
    from_stream_payload (payload, message);
}

template <typename TMessage>
void apply_packet_payload (TMessage &message, const std::vector<std::uint8_t> &payload, ...)
{
#if ZLINK_STREAM_CONNECTOR_HAS_EXCEPTIONS
    message = zlink::detail::json_profile::parse (payload.begin (), payload.end ())
                .template get<TMessage> ();
#else
    const auto parsed = zlink::detail::json_profile::try_parse (payload.begin (), payload.end ());
    if (parsed)
        message = parsed->template get<TMessage> ();
#endif
}

/* Decodes a wire payload into TMessage and reports a failure by value. A
 * user-supplied from_stream_payload may still throw where exceptions are
 * enabled, so the throwing form is caught here and turned into
 * frame_decode_failed instead of escaping the core boundary. */
template <typename TMessage>
result_t<TMessage> decode_typed_message (codec_t codec, const std::vector<std::uint8_t> &payload)
{
#if ZLINK_STREAM_CONNECTOR_HAS_EXCEPTIONS
    try {
        TMessage message{};
        apply_packet_payload (message, codec, payload, 0);
        return result_t<TMessage>::success (std::move (message));
    }
    catch (const std::exception &error) {
        return result_t<TMessage>::failure (error_code_t::frame_decode_failed, error.what ());
    }
    catch (...) {
        return result_t<TMessage>::failure (error_code_t::frame_decode_failed,
                                            "stream connector payload decode failed");
    }
#else
    if (codec == codec_t::json
        && !zlink::detail::json_profile::try_parse (payload.begin (), payload.end ())) {
        return result_t<TMessage>::failure (error_code_t::frame_decode_failed,
                                            "stream connector JSON payload decode failed");
    }
    TMessage message{};
    apply_packet_payload (message, codec, payload, 0);
    return result_t<TMessage>::success (std::move (message));
#endif
}

} // namespace zlink::stream_connector::detail

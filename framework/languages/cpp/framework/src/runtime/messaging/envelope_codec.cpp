/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/messaging/envelope_codec.hpp"

#include "runtime/diagnostics/flow_context.hpp"

#include <nlohmann/detail/exceptions.hpp>
#include <service_wire_constants.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#endif

namespace zlink::framework::runtime::messaging
{

message_parts_t::message_parts_t (zlink::message_t header, zlink::message_t body)
{
    constexpr std::size_t envelope_part_count = 2;
    _parts.reserve (envelope_part_count);
    _parts.push_back (std::move (header));
    _parts.push_back (std::move (body));
}

message_parts_t::message_parts_t (std::vector<zlink::message_t> parts) : _parts (std::move (parts))
{
}

const zlink::message_t &message_parts_t::operator[] (std::size_t index) const
{
    if (index >= _parts.size ()) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "ZLink envelope part is missing");
    }
    return _parts[index];
}

message_parts_t envelope_codec_t::encode_raw_body_parts (const envelope_header_t &header,
                                                         zlink::message_t body) const
{
    return message_parts_t (encode_header (header), std::move (body));
}

message_parts_t envelope_codec_t::encode_parts (const envelope_header_t &header,
                                                std::type_index body_type,
                                                const void *body,
                                                const serializer_registry_t &serializers) const
{
    if (body == nullptr) {
        return encode_raw_body_parts (header, zlink::message_t::from (""));
    }
    const auto content_type = serializers.content_type (body_type);
    auto encoded = detail::encoded_payload_to_raw (serializers.serialize (body_type, body));
    return message_parts_t (encode_header (header, content_type), std::move (encoded));
}

namespace
{

bool valid_error_code (const std::string &value) noexcept
{
    return value == "not_found" || value == "already_exists" || value == "type_mismatch"
           || value == "not_configured" || value == "rejected" || value == "unavailable"
           || value == "deadline_exceeded" || value == "shutting_down" || value == "protocol_error"
           || value == "invalid_operation" || value == "data_lost" || value == "internal_failure";
}

result_t<void> validate_protocol_header (const envelope_header_t &header,
                                         int format_marker,
                                         const std::string *flow_id,
                                         const std::optional<flow_origin_t> &flow_origin)
{
    if (header.kind < message_kind_t::request || header.kind > message_kind_t::error)
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "ZLink envelope header kind is invalid");

    if (!header.correlation_id.empty () && !header.can_correlate_reply ())
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "ZLink envelope correlationId must be 1..64 byte ASCII");
    if (format_marker != static_cast<int> (flow_id_t::format_marker)) {
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "ZLink envelope format marker is invalid");
    }
    if ((flow_id != nullptr) != flow_origin.has_value ()) {
        return result_t<void>::failure (
          framework_error_kind_t::protocol_error,
          "ZLink envelope flow id and origin must be present together");
    }
    if (flow_id && !flow_id_t::is_valid (*flow_id)) {
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "ZLink envelope flow id must be UUIDv7");
    }
    if (flow_origin) {
        const auto raw = static_cast<std::uint8_t> (*flow_origin);
        if (raw < 1 || raw > 4) {
            return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                            "ZLink envelope flow origin is invalid");
        }
    }
    if (header.kind == message_kind_t::error
        && (!header.error_code || !valid_error_code (*header.error_code))) {
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "ZLink error envelope requires a recognized errorCode");
    }
    return result_t<void>::success ();
}

// Inspect ordinary ASCII a word at a time. Quote, backslash, controls and
// non-ASCII bytes return to the same string grammar/UTF-8 owner below.
const char *plain_string_end (const char *first, const char *end) noexcept
{
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
    while (end - first >= static_cast<std::ptrdiff_t> (sizeof (__m128i))) {
        __m128i block;
        std::memcpy (&block, first, sizeof (block));
        // Signed byte comparison also selects all non-ASCII bytes. They go
        // through the same strict UTF-8 sequence validator as on other CPUs.
        const auto special =
          _mm_or_si128 (_mm_cmplt_epi8 (block, _mm_set1_epi8 (0x20)),
                        _mm_or_si128 (_mm_cmpeq_epi8 (block, _mm_set1_epi8 ('"')),
                                      _mm_cmpeq_epi8 (block, _mm_set1_epi8 ('\\'))));
        const auto mask = static_cast<unsigned> (_mm_movemask_epi8 (special));
        if (mask != 0)
            return first + std::countr_zero (mask);
        first += sizeof (block);
    }
#endif
    constexpr std::uint64_t low_bits = 0x0101010101010101ULL;
    constexpr std::uint64_t high_bits = 0x8080808080808080ULL;
    constexpr std::uint64_t spaces = 0x2020202020202020ULL;
    constexpr std::uint64_t quotes = 0x2222222222222222ULL;
    constexpr std::uint64_t slashes = 0x5c5c5c5c5c5c5c5cULL;
    while (end - first >= static_cast<std::ptrdiff_t> (sizeof (std::uint64_t))) {
        std::uint64_t word;
        std::memcpy (&word, first, sizeof (word));
        const auto quote = word ^ quotes;
        const auto slash = word ^ slashes;
        const auto special = ((quote - low_bits) & ~quote & high_bits)
                             | ((slash - low_bits) & ~slash & high_bits)
                             | ((word - spaces) & ~word & high_bits) | (word & high_bits);
        if (special != 0) {
            if constexpr (std::endian::native == std::endian::little)
                return first + std::countr_zero (special) / 8;
            break;
        }
        first += sizeof (word);
    }
    while (first != end) {
        const auto byte = static_cast<unsigned char> (*first);
        if (byte < 0x20 || byte >= 0x80 || byte == '"' || byte == '\\')
            break;
        ++first;
    }
    return first;
}

// A sequence length of zero means malformed UTF-8. The optional failure
// position is the offending byte, or end for an incomplete sequence.
std::size_t
utf8_sequence_length (const char *first, const char *end, const char **invalid = nullptr) noexcept
{
    const auto lead = static_cast<unsigned char> (*first);
    const std::size_t length = lead >= 0xc2 && lead <= 0xdf   ? 2
                               : lead >= 0xe0 && lead <= 0xef ? 3
                               : lead >= 0xf0 && lead <= 0xf4 ? 4
                                                              : 0;
    if (length == 0) {
        if (invalid)
            *invalid = first;
        return 0;
    }
    for (std::size_t index = 1; index < length; ++index) {
        if (first + index == end) {
            if (invalid)
                *invalid = end;
            return 0;
        }
        const auto byte = static_cast<unsigned char> (first[index]);
        if (byte < 0x80 || byte > 0xbf
            || (index == 1
                && ((lead == 0xe0 && byte < 0xa0) || (lead == 0xed && byte >= 0xa0)
                    || (lead == 0xf0 && byte < 0x90) || (lead == 0xf4 && byte >= 0x90)))) {
            if (invalid)
                *invalid = first + index;
            return 0;
        }
    }
    return length;
}

[[noreturn]] void invalid_utf8 (std::string_view value, const char *invalid)
{
    // Keep the writer's established nlohmann exception type, id and message.
    constexpr char digits[] = "0123456789ABCDEF";
    if (invalid == value.data () + value.size ()) {
        const auto last = static_cast<unsigned char> (value.back ());
        const char hex[] = {digits[last >> 4], digits[last & 15]};
        throw nlohmann::detail::type_error::create (
          316, "incomplete UTF-8 string; last byte: 0x" + std::string (hex, 2), nullptr);
    }
    const auto byte = static_cast<unsigned char> (*invalid);
    const char hex[] = {digits[byte >> 4], digits[byte & 15]};
    throw nlohmann::detail::type_error::create (316,
                                                "invalid UTF-8 byte at index "
                                                  + std::to_string (invalid - value.data ())
                                                  + ": 0x" + std::string (hex, 2),
                                                nullptr);
}

// The envelope has a fixed field order. Count the escaped bytes first, then
// write directly into its native message; there is no header DOM or byte copy.
enum class header_output_t
{
    measure,
    native_message,
    plan_string
};

// The three existing sinks use one writer grammar. Their output operation is
// selected at compile time instead of branching on pointers for every append.
template <header_output_t Output> class header_writer_t
{
  public:
    explicit header_writer_t (char *output = nullptr) : _output (output) {}
    explicit header_writer_t (std::string &output) : _string_output (&output) {}

    void append (std::string_view text)
    {
        if constexpr (Output == header_output_t::native_message) {
            if (!text.empty ())
                std::memcpy (_output + _size, text.data (), text.size ());
        } else if constexpr (Output == header_output_t::plan_string) {
            _string_output->append (text);
        }
        _size += text.size ();
    }

    void number (int value)
    {
        char buffer[std::numeric_limits<int>::digits10 + 3];
        const auto result = std::to_chars (buffer, buffer + sizeof (buffer), value);
        append (std::string_view (buffer, result.ptr - buffer));
    }

    void string (const std::string &value)
    {
        append ("\"");
        std::size_t start = 0;
        for (std::size_t index = 0; index < value.size ();) {
            index = static_cast<std::size_t> (
              plain_string_end (value.data () + index, value.data () + value.size ())
              - value.data ());
            if (index == value.size ())
                break;
            const auto byte = static_cast<unsigned char> (value[index]);
            if (byte >= 0x80) {
                if constexpr (Output != header_output_t::native_message) {
                    const char *invalid;
                    const auto length = utf8_sequence_length (
                      value.data () + index, value.data () + value.size (), &invalid);
                    if (length == 0)
                        invalid_utf8 (value, invalid);
                    index += length;
                } else {
                    ++index;
                }
                continue;
            }
            append (std::string_view (value).substr (start, index - start));
            switch (byte) {
                case '"':
                    append ("\\\"");
                    break;
                case '\\':
                    append ("\\\\");
                    break;
                case '\b':
                    append ("\\b");
                    break;
                case '\f':
                    append ("\\f");
                    break;
                case '\n':
                    append ("\\n");
                    break;
                case '\r':
                    append ("\\r");
                    break;
                case '\t':
                    append ("\\t");
                    break;
                default: {
                    constexpr char hex[] = "0123456789abcdef";
                    const char escaped[] = {'\\', 'u', '0', '0', hex[byte >> 4], hex[byte & 15]};
                    append (std::string_view (escaped, sizeof (escaped)));
                    break;
                }
            }
            start = ++index;
        }
        append (std::string_view (value).substr (start));
        append ("\"");
    }

    void optional_string (const std::optional<std::string> &value)
    {
        if (value)
            string (*value);
        else
            append ("null");
    }

    std::size_t size () const noexcept { return _size; }
    static constexpr bool measuring () noexcept
    {
        return Output != header_output_t::native_message;
    }

  private:
    char *_output = nullptr;
    std::string *_string_output = nullptr;
    std::size_t _size = 0;
};

template <header_output_t Output>
void write_metadata (header_writer_t<Output> &writer,
                     const std::map<std::string, std::string> &metadata)
{
    const auto start = writer.size ();
    writer.append ("{");
    bool first = true;
    for (const auto &[key, value] : metadata) {
        if (writer.measuring ()
            && (key.empty () || key.find ('\0') != std::string::npos
                || value.find ('\0') != std::string::npos))
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "ZLink envelope metadata key/value is invalid");
        if (!first)
            writer.append (",");
        first = false;
        try {
            writer.string (key);
            writer.append (":");
            writer.string (value);
        }
        catch (const nlohmann::detail::exception &error) {
            throw framework_exception_t (framework_error_kind_t::protocol_error, error.what ());
        }
    }
    writer.append ("}");
    if (writer.measuring () && writer.size () - start > protocol::metadataBytes)
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "ZLink envelope metadata exceeds "
                                       + std::to_string (protocol::metadataBytes) + " bytes");
}

struct header_plan_t
{
    std::optional<message_kind_t> kind;
    std::string channel_name;
    std::string message_name;
    std::string content_type;
    std::string prefix;
    std::string middle;

    bool matches (const envelope_header_t &header,
                  const std::string &selected_content_type) const noexcept
    {
        return kind && *kind == header.kind && channel_name == header.channel_name
               && message_name == header.message_name && content_type == selected_content_type;
    }

    void rebuild (const envelope_header_t &header, const std::string &selected_content_type)
    {
        // Keep the one-entry backing storage on this thread.  `kind` is the
        // validity marker and is published only after every escaped segment
        // and key has been rebuilt successfully.
        kind.reset ();
        channel_name.clear ();
        message_name.clear ();
        content_type.clear ();
        prefix.clear ();
        middle.clear ();

        channel_name = header.channel_name;
        message_name = header.message_name;
        content_type = selected_content_type;

        header_writer_t<header_output_t::plan_string> prefix_writer (prefix);
        prefix_writer.append ("{\"channelName\":");
        prefix_writer.string (channel_name);
        prefix_writer.append (",\"contentType\":");
        prefix_writer.string (content_type);
        prefix_writer.append (",\"correlationId\":");

        header_writer_t<header_output_t::plan_string> middle_writer (middle);
        middle_writer.append (",\"formatMarker\":");
        middle_writer.number (static_cast<int> (flow_id_t::format_marker));
        middle_writer.append (",\"kind\":");
        middle_writer.number (static_cast<int> (header.kind));
        middle_writer.append (",\"messageName\":");
        middle_writer.string (message_name);
        middle_writer.append (",\"metadata\":");
        kind = header.kind;
    }
};

header_plan_t &header_plan ()
{
    thread_local header_plan_t value;
    return value;
}

template <header_output_t Output>
void write_header (header_writer_t<Output> &writer,
                   const envelope_header_t &header,
                   const std::string *flow_id,
                   const std::optional<flow_origin_t> &flow_origin,
                   const header_plan_t &plan)
{
    writer.append (plan.prefix);
    if (header.correlation_id.empty ())
        writer.append ("null");
    else
        writer.string (header.correlation_id);
    writer.append (",\"deadline\":");
    writer.optional_string (header.deadline);
    writer.append (",\"errorCode\":");
    writer.optional_string (header.error_code);
    writer.append (",\"errorMessage\":");
    writer.optional_string (header.error_message);
    writer.append (",\"flowId\":");
    if (flow_id)
        writer.string (*flow_id);
    else
        writer.append ("null");
    writer.append (",\"flowOrigin\":");
    if (flow_origin)
        writer.number (static_cast<int> (*flow_origin));
    else
        writer.append ("null");
    writer.append (plan.middle);
    write_metadata (writer, header.metadata);
    writer.append (",\"source\":");
    writer.optional_string (header.source);
    writer.append (",\"topic\":");
    writer.optional_string (header.topic);
    writer.append ("}");
}

} // namespace

zlink::message_t envelope_codec_t::encode_header (const envelope_header_t &header) const
{
    return encode_header (header, header.content_type);
}

zlink::message_t envelope_codec_t::encode_header (const envelope_header_t &header,
                                                  const std::string &content_type) const
{
    const auto &flow = flow_context_t::current ();
    const bool stamp_flow = !header.flow_id && flow && !flow->flow_id.empty ();
    const auto *flow_id = stamp_flow ? &flow->flow_id : header.flow_id ? &*header.flow_id : nullptr;
    const auto flow_origin =
      stamp_flow ? std::optional<flow_origin_t> (flow->origin) : header.flow_origin;
    if (auto valid =
          validate_protocol_header (header, flow_id_t::format_marker, flow_id, flow_origin);
        !valid) {
        throw framework_exception_t (valid.error_kind (), valid.error ()->what ());
    }
    auto &plan = header_plan ();
    if (!plan.matches (header, content_type))
        plan.rebuild (header, content_type);
    header_writer_t<header_output_t::measure> measure;
    write_header (measure, header, flow_id, flow_origin, plan);
    auto message = zlink::message_t::allocate (measure.size ());
    if (!message.valid ())
        throw std::bad_alloc ();
    header_writer_t<header_output_t::native_message> writer (
      reinterpret_cast<char *> (message.data ()));
    write_header (writer, header, flow_id, flow_origin, plan);
    return message;
}

namespace
{

enum class header_value_t : unsigned char
{
    missing,
    null,
    string,
    number,
    other
};

// The reader owns JSON syntax and writes values directly into the returned
// header. Type tags retain the last occurrence without a second value model.
class header_reader_t
{
  public:
    header_reader_t (std::span<const std::byte> bytes,
                     bool capture_flow,
                     envelope_header_t &header) :
        _begin (bytes.empty () ? "" : reinterpret_cast<const char *> (bytes.data ())),
        _position (_begin),
        _end (_begin + bytes.size ()),
        _capture_flow (capture_flow),
        _header (header)
    {
    }

    void read (std::optional<envelope_header_t> *rejection_context)
    {
        if (_end - _position >= 3 && static_cast<unsigned char> (_position[0]) == 0xef
            && static_cast<unsigned char> (_position[1]) == 0xbb
            && static_cast<unsigned char> (_position[2]) == 0xbf)
            _position += 3;
        whitespace ();
        const bool object = _position != _end && *_position == '{';
        if (object) {
            ++_position;
            whitespace ();
            if (!consume ('}')) {
                do {
                    whitespace ();
                    const auto name = string (&_key, true);
                    whitespace ();
                    expect (':');
                    whitespace ();
                    field (name);
                    whitespace ();
                    if (consume ('}'))
                        break;
                    expect (',');
                } while (true);
            }
        } else {
            skip_value ();
        }
        whitespace ();
        // The established JSON lexer treats a raw NUL as end of input. This
        // applies only after a complete value, never inside strings/containers.
        if (_position != _end && *_position != '\0')
            syntax ("unexpected data after JSON value");
        if (!object)
            return failure ("ZLink envelope header must be a JSON object");
        return finish (rejection_context);
    }

  private:
    enum field_t : unsigned char
    {
        kind,
        channel_name,
        message_name,
        content_type,
        correlation_id,
        deadline,
        topic,
        error_code,
        error_message,
        source,
        flow_id,
        flow_origin,
        format_marker
    };

    enum class container_t : unsigned char
    {
        object_first,
        object_key,
        object_next,
        array_first,
        array_value,
        array_next
    };

    [[noreturn]] void syntax (std::string_view reason) const
    {
        throw std::runtime_error ("JSON parse error at byte "
                                  + std::to_string (_position - _begin + 1) + ": "
                                  + std::string (reason));
    }

    void whitespace () noexcept
    {
        while (_position != _end) {
            const auto byte = static_cast<unsigned char> (*_position);
            if (byte > ' ' || (byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n'))
                return;
            ++_position;
        }
    }

    bool consume (char value) noexcept
    {
        if (_position == _end || *_position != value)
            return false;
        ++_position;
        return true;
    }

    void expect (char value)
    {
        if (!consume (value))
            syntax ("unexpected JSON token");
    }

    static unsigned hex (unsigned char value) noexcept
    {
        if (value >= '0' && value <= '9')
            return value - '0';
        if (value >= 'a' && value <= 'f')
            return value - 'a' + 10;
        if (value >= 'A' && value <= 'F')
            return value - 'A' + 10;
        return 16;
    }

    unsigned code_unit ()
    {
        if (_end - _position < 4)
            syntax ("incomplete Unicode escape");
        unsigned value = 0;
        for (unsigned part = 0; part < 4; ++part) {
            const auto digit = hex (static_cast<unsigned char> (*_position++));
            if (digit == 16)
                syntax ("invalid Unicode escape");
            value = (value << 4) | digit;
        }
        return value;
    }

    static void append_codepoint (std::string &output, unsigned value)
    {
        if (value <= 0x7f) {
            output.push_back (static_cast<char> (value));
        } else if (value <= 0x7ff) {
            output.push_back (static_cast<char> (0xc0 | (value >> 6)));
            output.push_back (static_cast<char> (0x80 | (value & 0x3f)));
        } else if (value <= 0xffff) {
            output.push_back (static_cast<char> (0xe0 | (value >> 12)));
            output.push_back (static_cast<char> (0x80 | ((value >> 6) & 0x3f)));
            output.push_back (static_cast<char> (0x80 | (value & 0x3f)));
        } else {
            output.push_back (static_cast<char> (0xf0 | (value >> 18)));
            output.push_back (static_cast<char> (0x80 | ((value >> 12) & 0x3f)));
            output.push_back (static_cast<char> (0x80 | ((value >> 6) & 0x3f)));
            output.push_back (static_cast<char> (0x80 | (value & 0x3f)));
        }
    }

    // Plain keys borrow the input for classification. Values are owned by their
    // final destination; escaped keys alone use the reusable key string.
    std::string_view string (std::string *output, bool borrow_plain = false)
    {
        expect ('"');
        const char *start = _position;
        const char *segment = start;
        bool escaped = false;
        while (_position != _end) {
            _position = plain_string_end (_position, _end);
            if (_position == _end)
                break;
            const auto byte = static_cast<unsigned char> (*_position);
            if (byte == '"') {
                const auto raw = std::string_view (start, _position - start);
                if (output) {
                    if (escaped)
                        output->append (segment, _position - segment);
                    else if (!borrow_plain)
                        output->assign (raw);
                }
                ++_position;
                return escaped && output ? std::string_view (*output) : raw;
            }
            if (byte < 0x20)
                syntax ("unescaped control character in string");
            if (byte >= 0x80) {
                const auto length = utf8_sequence_length (_position, _end);
                if (length == 0)
                    syntax ("invalid UTF-8 string");
                _position += length;
                continue;
            }
            if (output) {
                if (!escaped)
                    output->assign (segment, _position - segment);
                else
                    output->append (segment, _position - segment);
            }
            escaped = true;
            ++_position;
            if (_position == _end)
                syntax ("incomplete string escape");
            const char escape = *_position++;
            char decoded;
            switch (escape) {
                case '"':
                    decoded = '"';
                    break;
                case '\\':
                    decoded = '\\';
                    break;
                case '/':
                    decoded = '/';
                    break;
                case 'b':
                    decoded = '\b';
                    break;
                case 'f':
                    decoded = '\f';
                    break;
                case 'n':
                    decoded = '\n';
                    break;
                case 'r':
                    decoded = '\r';
                    break;
                case 't':
                    decoded = '\t';
                    break;
                case 'u': {
                    unsigned value = code_unit ();
                    if (value >= 0xd800 && value <= 0xdbff) {
                        expect ('\\');
                        expect ('u');
                        const auto low = code_unit ();
                        if (low < 0xdc00 || low > 0xdfff)
                            syntax ("invalid Unicode surrogate pair");
                        value = 0x10000 + ((value - 0xd800) << 10) + low - 0xdc00;
                    } else if (value >= 0xdc00 && value <= 0xdfff) {
                        syntax ("unpaired Unicode surrogate");
                    }
                    if (output)
                        append_codepoint (*output, value);
                    segment = _position;
                    continue;
                }
                default:
                    syntax ("invalid string escape");
            }
            if (output)
                output->push_back (decoded);
            segment = _position;
        }
        syntax ("unterminated JSON string");
    }

    void literal (std::string_view value)
    {
        if (static_cast<std::size_t> (_end - _position) < value.size ()
            || std::string_view (_position, value.size ()) != value)
            syntax ("invalid JSON literal");
        _position += value.size ();
    }

    static bool digit (char value) noexcept { return value >= '0' && value <= '9'; }

    // from_chars reports both finite underflow and non-finite overflow as
    // out_of_range. For those values alone, decimal order distinguishes them
    // without a locale-dependent conversion or another number parser.
    static bool underflows (const char *first, const char *last) noexcept
    {
        if (*first == '-')
            ++first;
        const auto limit = static_cast<std::size_t> (last - first);
        std::size_t digits_before_point = 0;
        std::size_t leading_zeroes = 0;
        bool point = false;
        bool significant = false;
        while (first != last && *first != 'e' && *first != 'E') {
            if (*first == '.')
                point = true;
            else {
                if (!point)
                    ++digits_before_point;
                if (!significant && *first == '0')
                    ++leading_zeroes;
                else
                    significant = true;
            }
            ++first;
        }
        if (!significant)
            return true;
        const auto order = static_cast<std::ptrdiff_t> (digits_before_point)
                           - static_cast<std::ptrdiff_t> (leading_zeroes) - 1;
        bool negative = false;
        std::size_t exponent = 0;
        if (first != last) {
            ++first;
            if (first != last && (*first == '+' || *first == '-'))
                negative = *first++ == '-';
            while (first != last) {
                const auto next = static_cast<std::size_t> (*first++ - '0');
                if (next > limit || exponent > (limit - next) / 10)
                    exponent = limit;
                else
                    exponent = exponent * 10 + next;
            }
        }
        if (negative)
            return order < 0 || exponent > static_cast<std::size_t> (order);
        return order < 0 && exponent < static_cast<std::size_t> (-(order + 1)) + 1;
    }

    header_value_t number (int *output)
    {
        const char *start = _position;
        const bool negative = consume ('-');
        if (_position == _end || !digit (*_position))
            syntax ("invalid JSON number");
        if (consume ('0')) {
            if (_position != _end && digit (*_position))
                syntax ("leading zero in JSON number");
        } else {
            while (_position != _end && digit (*_position))
                ++_position;
        }
        bool integer = true;
        if (consume ('.')) {
            integer = false;
            if (_position == _end || !digit (*_position))
                syntax ("missing JSON fraction");
            while (_position != _end && digit (*_position))
                ++_position;
        }
        if (_position != _end && (*_position == 'e' || *_position == 'E')) {
            integer = false;
            ++_position;
            if (_position != _end && (*_position == '+' || *_position == '-'))
                ++_position;
            if (_position == _end || !digit (*_position))
                syntax ("missing JSON exponent");
            while (_position != _end && digit (*_position))
                ++_position;
        }
        if (integer) {
            std::uint64_t magnitude = 0;
            const auto result = std::from_chars (start + negative, _position, magnitude);
            if (result.ec == std::errc{}) {
                const auto maximum = static_cast<std::uint64_t> (std::numeric_limits<int>::max ());
                if (magnitude <= maximum + negative) {
                    if (output)
                        *output = negative
                                    ? static_cast<int> (-static_cast<std::int64_t> (magnitude))
                                    : static_cast<int> (magnitude);
                    return header_value_t::number;
                }
                // nlohmann promotes negative integers below int64 to double.
                if (!negative || magnitude <= (std::uint64_t{1} << 63))
                    return header_value_t::other;
            }
        }
        double value;
        const auto converted = std::from_chars (start, _position, value);
        if (converted.ec == std::errc::result_out_of_range) {
            if (!underflows (start, _position))
                syntax ("JSON number overflow");
            value = 0;
        } else if (converted.ec != std::errc{} || converted.ptr != _position) {
            syntax ("invalid JSON number");
        }
        if (!std::isfinite (value))
            syntax ("JSON number overflow");
        return header_value_t::other;
    }

    void atom (std::vector<container_t> &stack)
    {
        if (_position == _end)
            syntax ("missing JSON value");
        switch (*_position) {
            case '{':
                ++_position;
                stack.push_back (container_t::object_first);
                break;
            case '[':
                ++_position;
                stack.push_back (container_t::array_first);
                break;
            case '"':
                string (nullptr);
                break;
            case 'n':
                literal ("null");
                break;
            case 't':
                literal ("true");
                break;
            case 'f':
                literal ("false");
                break;
            default:
                number (nullptr);
                break;
        }
    }

    void skip_value ()
    {
        std::vector<container_t> stack;
        atom (stack);
        while (!stack.empty ()) {
            whitespace ();
            switch (stack.back ()) {
                case container_t::object_first:
                    if (consume ('}')) {
                        stack.pop_back ();
                        break;
                    }
                    [[fallthrough]];
                case container_t::object_key:
                    string (nullptr);
                    whitespace ();
                    expect (':');
                    whitespace ();
                    stack.back () = container_t::object_next;
                    atom (stack);
                    break;
                case container_t::object_next:
                    if (consume ('}'))
                        stack.pop_back ();
                    else {
                        expect (',');
                        stack.back () = container_t::object_key;
                    }
                    break;
                case container_t::array_first:
                    if (consume (']')) {
                        stack.pop_back ();
                        break;
                    }
                    [[fallthrough]];
                case container_t::array_value:
                    stack.back () = container_t::array_next;
                    atom (stack);
                    break;
                case container_t::array_next:
                    if (consume (']'))
                        stack.pop_back ();
                    else {
                        expect (',');
                        stack.back () = container_t::array_value;
                    }
                    break;
            }
        }
    }

    static constexpr std::size_t field_count = static_cast<std::size_t> (format_marker) + 1;

    header_value_t type (field_t field) const noexcept { return _types[field]; }

    void type (field_t field, header_value_t value) noexcept { _types[field] = value; }

    header_value_t scalar (std::string *text, int *integer = nullptr)
    {
        if (_position == _end)
            syntax ("missing JSON value");
        if (*_position == '"') {
            string (text);
            return header_value_t::string;
        }
        if (text)
            text->clear ();
        if (*_position == 'n') {
            literal ("null");
            return header_value_t::null;
        }
        if (*_position == '-' || digit (*_position))
            return number (integer);
        skip_value ();
        return header_value_t::other;
    }

    void text (field_t field, std::string &destination)
    {
        const auto value = scalar (&destination);
        type (field, value == header_value_t::number ? header_value_t::other : value);
    }

    void text (field_t field, std::optional<std::string> &destination)
    {
        if (_position != _end && *_position == '"') {
            if (!destination)
                destination.emplace ();
            string (&*destination);
            type (field, header_value_t::string);
        } else {
            destination.reset ();
            const auto value = scalar (nullptr);
            type (field, value == header_value_t::number ? header_value_t::other : value);
        }
    }

    void integer (field_t field, int &destination)
    {
        // A string on an integer field still has a string type tag, but never
        // materializes a string that cannot enter the final header.
        type (field, scalar (nullptr, &destination));
    }

    void metadata ()
    {
        _header.metadata.clear ();
        _invalid_metadata_keys.clear ();
        _invalid_metadata_shape = !consume ('{');
        if (_invalid_metadata_shape) {
            skip_value ();
            return;
        }
        whitespace ();
        if (consume ('}'))
            return;
        do {
            whitespace ();
            // Metadata keys must be owned by their final map entry.
            std::string key;
            string (&key);
            whitespace ();
            expect (':');
            whitespace ();
            if (_position != _end && *_position == '"') {
                _invalid_metadata_keys.erase (
                  std::remove (_invalid_metadata_keys.begin (), _invalid_metadata_keys.end (), key),
                  _invalid_metadata_keys.end ());
                auto [entry, inserted] = _header.metadata.try_emplace (std::move (key));
                (void) inserted;
                string (&entry->second);
            } else {
                _header.metadata.erase (key);
                _invalid_metadata_keys.push_back (std::move (key));
                skip_value ();
            }
            whitespace ();
            if (consume ('}'))
                break;
            expect (',');
        } while (true);
    }

    void field (std::string_view name)
    {
        // This is the only key-to-destination mapping. No member enum is
        // translated a second time when the value arrives.
        using namespace std::string_view_literals;
        switch (name.size ()) {
            case 4:
                if (name == "kind"sv) {
                    int value = static_cast<int> (_header.kind);
                    integer (kind, value);
                    _header.kind = static_cast<message_kind_t> (value);
                    return;
                }
                break;
            case 5:
                if (name == "topic"sv)
                    return text (topic, _header.topic);
                break;
            case 6:
                if (name == "source"sv)
                    return text (source, _header.source);
                if (name == "flowId"sv && _capture_flow)
                    return text (flow_id, _header.flow_id);
                break;
            case 8:
                if (name == "deadline"sv)
                    return text (deadline, _header.deadline);
                if (name == "metadata"sv)
                    return metadata ();
                break;
            case 9:
                if (name == "errorCode"sv)
                    return text (error_code, _header.error_code);
                break;
            case 10:
                if (name == "flowOrigin"sv && _capture_flow)
                    return integer (flow_origin, _flow_origin);
                break;
            case 11:
                if (name == "channelName"sv)
                    return text (channel_name, _header.channel_name);
                if (name == "messageName"sv)
                    return text (message_name, _header.message_name);
                if (name == "contentType"sv)
                    return text (content_type, _header.content_type);
                break;
            case 12:
                if (name == "formatMarker"sv)
                    return integer (format_marker, _format_marker);
                if (name == "errorMessage"sv)
                    return text (error_message, _header.error_message);
                break;
            case 13:
                if (name == "correlationId"sv)
                    return text (correlation_id, _header.correlation_id);
                break;
        }
        skip_value ();
    }

    bool optional_string (field_t field) const noexcept
    {
        const auto value = type (field);
        return value == header_value_t::missing || value == header_value_t::null
               || value == header_value_t::string;
    }

    bool number_or_default (field_t field) const noexcept
    {
        const auto value = type (field);
        return value == header_value_t::missing || value == header_value_t::null
               || value == header_value_t::number;
    }

    void finish (std::optional<envelope_header_t> *rejection_context)
    {
        if (type (content_type) != header_value_t::string)
            _header.content_type = envelope_codec_t::default_content_type;
        const auto reject = [&] (std::string_view message, bool retain_flow = false) {
            if (!retain_flow) {
                _header.flow_id.reset ();
                _header.flow_origin.reset ();
            }
            _header.metadata.clear ();
            if (rejection_context)
                *rejection_context = std::move (_header);
            return failure (message);
        };
        if (type (kind) != header_value_t::number || type (channel_name) != header_value_t::string
            || type (message_name) != header_value_t::string)
            return reject ("ZLink envelope header has invalid required fields");
        if (type (content_type) != header_value_t::missing
            && type (content_type) != header_value_t::string)
            return reject ("ZLink envelope header contentType is invalid");
        if (!optional_string (correlation_id) || !optional_string (deadline)
            || !optional_string (topic) || !optional_string (error_code)
            || !optional_string (error_message) || !optional_string (source)
            || !number_or_default (format_marker))
            return reject ("ZLink envelope header has an invalid field type");
        if (_capture_flow && (!optional_string (flow_id) || !number_or_default (flow_origin))) {
            _header.flow_id.reset ();
            return reject ("ZLink envelope header flow fields are invalid");
        }
        if (_capture_flow && type (flow_origin) == header_value_t::number)
            _header.flow_origin = static_cast<flow_origin_t> (_flow_origin);
        const auto marker = type (format_marker) == header_value_t::number ? _format_marker : 0;
        if (auto valid = validate_protocol_header (
              _header, marker, _header.flow_id ? &*_header.flow_id : nullptr, _header.flow_origin);
            !valid) {
            _header.flow_id.reset ();
            _header.flow_origin.reset ();
            return reject (valid.error ()->what ());
        }
        if (_invalid_metadata_shape)
            return reject ("ZLink envelope header metadata must be an object", true);
        if (!_invalid_metadata_keys.empty ())
            return reject ("ZLink envelope header metadata value is invalid", true);
        header_writer_t<header_output_t::measure> measure;
        try {
            write_metadata (measure, _header.metadata);
        }
        catch (...) {
            _header.metadata.clear ();
            if (rejection_context)
                *rejection_context = std::move (_header);
            throw;
        }
    }

    [[noreturn]] static void failure (std::string_view reason)
    {
        throw std::runtime_error (std::string (reason));
    }

    const char *_begin;
    const char *_position;
    const char *_end;
    bool _capture_flow;
    envelope_header_t &_header;
    std::array<header_value_t, field_count> _types{};
    int _format_marker = 0;
    int _flow_origin = 0;
    std::string _key;
    bool _invalid_metadata_shape = false;
    std::vector<std::string> _invalid_metadata_keys;
};

} // namespace

bool envelope_header_t::can_correlate_reply () const noexcept
{
    constexpr std::size_t maximum_correlation_id_bytes = 64;
    constexpr unsigned char maximum_ascii_character = 0x7f;
    return !correlation_id.empty () && correlation_id.size () <= maximum_correlation_id_bytes
           && std::all_of (correlation_id.begin (), correlation_id.end (),
                           [] (unsigned char value) { return value <= maximum_ascii_character; });
}

result_t<envelope_header_t>
envelope_codec_t::decode_header (const zlink::message_t &message,
                                 bool capture_flow,
                                 std::optional<envelope_header_t> *rejection_context) const
{
    if (rejection_context)
        rejection_context->reset ();
    const auto failure = [] (const std::exception &error) {
        return result_t<envelope_header_t>::failure (framework_error_kind_t::protocol_error,
                                                     std::string ("invalid ZLink envelope header: ")
                                                       + error.what ());
    };
    try {
        // One result owns the header throughout decoding. The outer boundary
        // also covers platforms whose empty map construction can allocate.
        return [&] {
            auto result = result_t<envelope_header_t>::success (
              envelope_header_t{.kind = static_cast<message_kind_t> (0), .content_type = {}});
            try {
                const auto bytes = message.bytes ();
                header_reader_t reader (bytes, capture_flow, result.value ());
                reader.read (rejection_context);
            }
            catch (const std::exception &error) {
                result = failure (error);
            }
            return result;
        }();
    }
    catch (const std::exception &error) {
        return failure (error);
    }
}

result_t<envelope_header_t>
envelope_codec_t::decode_header (const message_parts_t &parts,
                                 bool capture_flow,
                                 std::optional<envelope_header_t> *rejection_context) const
{
    if (parts.size () == 0) {
        if (rejection_context)
            rejection_context->reset ();
        return result_t<envelope_header_t>::failure (framework_error_kind_t::protocol_error,
                                                     "ZLink envelope header part is missing");
    }
    return decode_header (parts[0], capture_flow, rejection_context);
}

result_t<zlink::message_t> envelope_codec_t::decode_body (const message_parts_t &parts) const
{
    if (parts.size () < 2) {
        return result_t<zlink::message_t>::failure (framework_error_kind_t::protocol_error,
                                                    "ZLink envelope body part is missing");
    }
    return result_t<zlink::message_t>::success (parts[1].copy ());
}

} // namespace zlink::framework::runtime::messaging

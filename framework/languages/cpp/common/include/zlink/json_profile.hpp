/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <nlohmann/json.hpp>
#include <zlink/json.hpp>
#include <charconv>
#include <cstddef>
#include <limits>
#include <type_traits>
#include <vector>

#include <cmath>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>

#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
#define ZLINK_HAS_EXCEPTIONS 1
#else
#define ZLINK_HAS_EXCEPTIONS 0
#endif

namespace zlink::detail::json_profile
{

inline constexpr char content_type[] = "application/json";
inline constexpr char empty_object[] = "{}";
inline constexpr std::size_t initial_output_capacity = 256;
inline constexpr std::size_t initial_frame_capacity = 4;
inline constexpr std::size_t field_mask_bits = std::numeric_limits<std::uint64_t>::digits;

template <typename T = double> bool valid_float (double value)
{
    return std::isfinite (value) && value <= std::numeric_limits<T>::max ()
           && value >= -std::numeric_limits<T>::max ();
}

template <typename T, typename Match, typename Visit>
bool visit_enum_entry (Match match, Visit visit)
{
    for (const auto entry : zlink::json::enum_traits<T>::values)
        if (match (entry)) {
            visit (entry);
            return true;
        }
    return false;
}

inline bool has_finite_numbers (const nlohmann::json &value)
{
    if (value.is_number_float () && !valid_float (value.get<double> ())) {
        return false;
    }
    if (value.is_array ()) {
        for (const auto &element : value)
            if (!has_finite_numbers (element))
                return false;
    }
    if (value.is_object ()) {
        for (const auto &[_, element] : value.items ())
            if (!has_finite_numbers (element))
                return false;
    }
    return true;
}

template <typename T, typename Iterator>
std::optional<T> try_decode (Iterator begin, Iterator end, std::string *error = nullptr);

template <typename TIterator>
std::optional<nlohmann::json>
try_parse (TIterator begin, TIterator end, std::string *error = nullptr)
{
    return try_decode<nlohmann::json> (begin, end, error);
}

#if ZLINK_HAS_EXCEPTIONS
template <typename TIterator> nlohmann::json parse (TIterator begin, TIterator end)
{
    std::string error;
    auto parsed = try_parse (begin, end, &error);
    if (!parsed)
        throw std::invalid_argument (error);
    return std::move (*parsed);
}
#endif

inline std::string dump (const nlohmann::json &value)
{
    if (!has_finite_numbers (value)) {
#if ZLINK_HAS_EXCEPTIONS
        throw std::invalid_argument ("framework-json-v1 rejects non-finite numbers");
#else
        return {};
#endif
    }
    return value.dump ();
}

} // namespace zlink::detail::json_profile

namespace zlink::detail::json_profile
{
template <typename T> struct optional_type : std::false_type
{
};
template <typename T> struct optional_type<std::optional<T>> : std::true_type
{
    using value_type = T;
};
template <typename T> struct vector_type : std::false_type
{
};
template <typename T, typename A> struct vector_type<std::vector<T, A>> : std::true_type
{
    using value_type = T;
};
template <typename T> struct map_type : std::false_type
{
};
template <typename T, typename C, typename A>
struct map_type<std::map<std::string, T, C, A>> : std::true_type
{
    using value_type = T;
};
template <typename T>
inline constexpr bool has_dto = requires { zlink::json::dto_traits<T>::fields; };
template <typename T>
inline constexpr bool has_enum = requires { zlink::json::enum_traits<T>::values; };
template <typename T> consteval bool is_json_candidate ()
{
    if constexpr (std::is_same_v<T, nlohmann::json> || std::is_same_v<T, bool>
                  || std::is_same_v<T, std::string> || std::is_same_v<T, float>
                  || std::is_same_v<T, double> || std::is_same_v<T, std::vector<std::byte>>
                  || (std::is_integral_v<T> && sizeof (T) <= 8))
        return true;
    else if constexpr (optional_type<T>::value)
        return is_json_candidate<typename optional_type<T>::value_type> ();
    else if constexpr (vector_type<T>::value)
        return is_json_candidate<typename vector_type<T>::value_type> ();
    else if constexpr (map_type<T>::value)
        return is_json_candidate<typename map_type<T>::value_type> ();
    else if constexpr (std::is_enum_v<T>)
        return requires { sizeof (zlink::json::enum_traits<T>); };
    else
        return requires { sizeof (zlink::json::dto_traits<T>); };
}
template <typename T> inline constexpr bool dependent_false = false;
template <typename T> struct member_type;
template <typename C, typename M> struct member_type<M C::*>
{
    using owner = C;
    using value = M;
};

constexpr bool valid_enum_name (std::string_view name)
{
    if (name.empty ())
        return false;
    // Numeric ordinal strings are never enum names (message model §5).
    auto cursor = name.begin ();
    if (*cursor == '-' || *cursor == '+')
        ++cursor;
    bool numeric = cursor != name.end ();
    for (; cursor != name.end (); ++cursor)
        numeric = numeric && *cursor >= '0' && *cursor <= '9';
    return !numeric;
}

template <typename T, typename... Seen> consteval bool validate_type ()
{
    if constexpr ((std::is_same_v<T, Seen> || ...))
        return true;
    else if constexpr (std::is_same_v<T, nlohmann::json>) {
        static_assert (sizeof...(Seen) == 0,
                       "framework-json-v1: JSON DOM is only supported as a top-level payload");
        return true;
    } else if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, std::string>
                         || std::is_same_v<T, float> || std::is_same_v<T, double>
                         || std::is_same_v<T, std::vector<std::byte>>)
        return true;
    else if constexpr (std::is_integral_v<T>) {
        static_assert (sizeof (T) <= 8, "framework-json-v1: unsupported integer width");
        return true;
    } else if constexpr (optional_type<T>::value || vector_type<T>::value || map_type<T>::value) {
        if constexpr (optional_type<T>::value)
            return validate_type<typename optional_type<T>::value_type, Seen..., T> ();
        else if constexpr (vector_type<T>::value)
            return validate_type<typename vector_type<T>::value_type, Seen..., T> ();
        else
            return validate_type<typename map_type<T>::value_type, Seen..., T> ();
    } else if constexpr (std::is_enum_v<T>) {
        static_assert (has_enum<T>, "framework-json-v1: enum_traits is required");
        if constexpr (has_enum<T>) {
            constexpr auto values = zlink::json::enum_traits<T>::values;
            static_assert (std::is_same_v<std::remove_cvref_t<decltype (values[0].value)>, T>,
                           "framework-json-v1: enum descriptor must name its declared enum type");
            static_assert (
              [=] {
                  for (std::size_t i = 0; i < values.size (); ++i) {
                      if (!valid_enum_name (values[i].name))
                          return false;
                      for (std::size_t j = 0; j < i; ++j)
                          if (values[i].name == values[j].name
                              || values[i].value == values[j].value)
                              return false;
                  }
                  return true;
              }(),
              "framework-json-v1: enum names and values must be unique; ordinal names are "
              "forbidden");
        }
        return true;
    } else {
        static_assert (has_dto<T>,
                       "framework-json-v1: dto_traits is required; unsupported field type");
        if constexpr (has_dto<T>) {
            static_assert (std::is_default_constructible_v<T>,
                           "framework-json-v1: DTO must be default constructible");
            constexpr auto fields = zlink::json::dto_traits<T>::fields;
            static_assert (
              [=] {
                  return std::apply (
                    [] (auto... f) {
                        std::array<std::string_view, sizeof...(f)> names{f.name...};
                        for (std::size_t i = 0; i < names.size (); ++i)
                            for (std::size_t j = 0; j < i; ++j)
                                if (names[i] == names[j])
                                    return false;
                        return true;
                    },
                    fields);
              }(),
              "framework-json-v1: duplicate wire property name");
            std::apply (
              [] (auto... f) {
                  (
                    []<typename F> (F) {
                        using member = member_type<decltype (F::member)>;
                        static_assert (std::is_same_v<typename member::owner, T>,
                                       "framework-json-v1: field must be declared directly by DTO");
                        static_assert (!std::is_const_v<typename member::value>,
                                       "framework-json-v1: const member is unsupported");
                        static_assert (validate_type<typename member::value, Seen..., T> ());
                    }(f),
                    ...);
              },
              fields);
        }
        return true;
    }
}

inline bool fail (std::string *error, const char *message)
{
    if (error)
        *error = message;
    return false;
}

inline constexpr std::string_view base64_alphabet =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
inline std::string encode_bytes (const std::vector<std::byte> &bytes)
{
    std::string text;
    text.reserve ((bytes.size () / 3 + (bytes.size () % 3 != 0)) * 4);
    for (std::size_t i = 0; i < bytes.size (); i += 3) {
        const unsigned a = std::to_integer<unsigned> (bytes[i]);
        const unsigned b = i + 1 < bytes.size () ? std::to_integer<unsigned> (bytes[i + 1]) : 0;
        const unsigned c = i + 2 < bytes.size () ? std::to_integer<unsigned> (bytes[i + 2]) : 0;
        text += base64_alphabet[a >> 2];
        text += base64_alphabet[((a & 3) << 4) | (b >> 4)];
        text += i + 1 < bytes.size () ? base64_alphabet[((b & 15) << 2) | (c >> 6)] : '=';
        text += i + 2 < bytes.size () ? base64_alphabet[c & 63] : '=';
    }
    return text;
}
inline bool decode_bytes (std::string_view text, std::vector<std::byte> &bytes)
{
    if (text.size () % 4)
        return false;
    bytes.clear ();
    for (std::size_t i = 0; i < text.size (); i += 4) {
        unsigned v[4]{};
        for (unsigned j = 0; j < 4; ++j) {
            if (text[i + j] == '=' && j >= 2 && i + 4 == text.size ())
                continue;
            const auto n = base64_alphabet.find (text[i + j]);
            if (n == std::string_view::npos)
                return false;
            v[j] = static_cast<unsigned> (n);
        }
        if (text[i + 2] == '=' && (text[i + 3] != '=' || (v[1] & 15) != 0))
            return false;
        if (text[i + 3] == '=' && (v[2] & 3) != 0)
            return false;
        bytes.push_back (std::byte ((v[0] << 2) | (v[1] >> 4)));
        if (text[i + 2] != '=')
            bytes.push_back (std::byte ((v[1] << 4) | (v[2] >> 2)));
        if (text[i + 3] != '=')
            bytes.push_back (std::byte ((v[2] << 6) | v[3]));
    }
    return true;
}

inline bool valid_utf8 (std::string_view text)
{
    for (std::size_t i = 0; i < text.size ();) {
        const auto first = static_cast<unsigned char> (text[i++]);
        if (first < 0x80)
            continue;
        unsigned count = 0, value = 0, minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            count = 1;
            value = first & 0x1f;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            count = 2;
            value = first & 0x0f;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            count = 3;
            value = first & 7;
            minimum = 0x10000;
        } else
            return false;
        if (count > text.size () - i)
            return false;
        while (count--) {
            const auto next = static_cast<unsigned char> (text[i++]);
            if ((next & 0xc0) != 0x80)
                return false;
            value = (value << 6) | (next & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
            return false;
    }
    return true;
}
inline bool has_valid_strings (const nlohmann::json &value)
{
    if (value.is_string ())
        return valid_utf8 (value.get_ref<const std::string &> ());
    if (value.is_array ()) {
        for (const auto &item : value)
            if (!has_valid_strings (item))
                return false;
    } else if (value.is_object ()) {
        for (const auto &[key, item] : value.items ())
            if (!valid_utf8 (key) || !has_valid_strings (item))
                return false;
    }
    return true;
}

class byte_output
{
    std::vector<std::uint8_t> &bytes;

  public:
    explicit byte_output (std::vector<std::uint8_t> &out) : bytes (out) {}
    void reserve (std::size_t size) { bytes.reserve (size); }
    void operator+= (char value) { bytes.push_back (static_cast<std::uint8_t> (value)); }
    void operator+= (std::string_view value)
    {
        bytes.insert (bytes.end (), value.begin (), value.end ());
    }
    void append (const char *begin, const char *end) { bytes.insert (bytes.end (), begin, end); }
};

template <typename Output>
bool write_string (std::string_view value, Output &out, std::string *error)
{
    if (!valid_utf8 (value))
        return fail (error, "invalid UTF-8 string");
    constexpr char hex[] = "0123456789abcdef";
    out += '"';
    for (const unsigned char ch : value) {
        switch (ch) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (ch < 0x20) {
                    out += "\\u00";
                    out += hex[ch >> 4];
                    out += hex[ch & 15];
                } else
                    out += static_cast<char> (ch);
        }
    }
    out += '"';
    return true;
}

template <typename T, typename Output> void write_integer (T value, Output &out)
{
    char buffer[32];
    auto result = std::to_chars (buffer, buffer + sizeof buffer, value);
    out.append (buffer, result.ptr);
}

template <typename T> bool read_decimal (std::string_view text, T &out, std::string *error)
{
    if (text.empty () || text.front () == '+' || text == "-0")
        return fail (error, "invalid decimal string");
    const auto start = text.front () == '-' ? 1u : 0u;
    if (start == text.size () || (text[start] == '0' && text.size () - start > 1))
        return fail (error, "invalid decimal string");
    for (std::size_t i = start; i < text.size (); ++i)
        if (text[i] < '0' || text[i] > '9')
            return fail (error, "invalid decimal string");
    const auto result = std::from_chars (text.data (), text.data () + text.size (), out);
    return (result.ec == std::errc{} && result.ptr == text.data () + text.size ())
           || fail (error, "64-bit integer out of range");
}

template <typename T> struct dto_layout
{
    static constexpr auto fields = zlink::json::dto_traits<T>::fields;
    static constexpr std::size_t count = std::tuple_size_v<decltype (fields)>;
    static constexpr auto names = std::apply (
      [] (auto... field) { return std::array<std::string_view, count>{field.name...}; }, fields);
    static constexpr auto order = [] {
        std::array<std::size_t, count> indices{};
        for (std::size_t i = 0; i < count; ++i)
            indices[i] = i;
        std::sort (indices.begin (), indices.end (),
                   [] (auto a, auto b) { return names[a] < names[b]; });
        return indices;
    }();
    static std::size_t find (std::string_view key)
    {
        const auto it =
          std::lower_bound (order.begin (), order.end (), key,
                            [] (auto index, auto name) { return names[index] < name; });
        return it != order.end () && names[*it] == key ? *it : count;
    }
};

template <typename Output> void write_float (double value, Output &out)
{
    // Message model §5 fixes only "finite JSON number"; the text is the shortest
    // round-trip form. Negative zero keeps a fraction so a reader sees a float.
    if (value == 0 && std::signbit (value)) {
        out += "-0.0";
        return;
    }
    if (std::trunc (value) == value && value >= -9007199254740991.0
        && value <= 9007199254740991.0) {
        write_integer (static_cast<std::int64_t> (value), out);
        return;
    }
    char buffer[64];
    const auto converted = std::to_chars (buffer, buffer + sizeof buffer, value);
    out.append (buffer, converted.ptr);
}
template <typename T, typename Output>
bool write_value (const T &value, Output &out, std::string *error)
{
    static_assert (validate_type<T> ());
    if constexpr (std::is_same_v<T, nlohmann::json>) {
        if (!has_finite_numbers (value) || !has_valid_strings (value))
            return fail (error, "invalid JSON value");
        out += dump (value);
    } else if constexpr (optional_type<T>::value) {
        if (value)
            return write_value (*value, out, error);
        out += "null";
    } else if constexpr (std::is_same_v<T, std::string>)
        return write_string (value, out, error);
    else if constexpr (std::is_same_v<T, bool>)
        out += value ? "true" : "false";
    else if constexpr (std::is_integral_v<T>) {
        if constexpr (sizeof (T) == 8)
            out += '"';
        write_integer (value, out);
        if constexpr (sizeof (T) == 8)
            out += '"';
    } else if constexpr (std::is_floating_point_v<T>) {
        if (!valid_float (value))
            return fail (error, "non-finite number");
        write_float (static_cast<double> (value), out);
    } else if constexpr (std::is_enum_v<T>) {
        bool written = false;
        const auto found = visit_enum_entry<T> (
          [&] (auto entry) { return entry.value == value; },
          [&] (auto entry) { written = write_string (entry.name, out, error); });
        return (found && written) || fail (error, "undeclared enum value");
    } else if constexpr (std::is_same_v<T, std::vector<std::byte>>)
        return write_string (encode_bytes (value), out, error);
    else if constexpr (vector_type<T>::value) {
        out += '[';
        bool first = true;
        for (const auto &element : value) {
            if (!first)
                out += ',';
            first = false;
            if (!write_value<typename vector_type<T>::value_type> (element, out, error))
                return false;
        }
        out += ']';
    } else if constexpr (map_type<T>::value) {
        out += '{';
        bool first = true;
        // JSON object order is lexical even when the application's map comparator differs.
        std::vector<const typename T::value_type *> entries;
        entries.reserve (value.size ());
        for (const auto &entry : value)
            entries.push_back (&entry);
        std::sort (entries.begin (), entries.end (),
                   [] (auto a, auto b) { return a->first < b->first; });
        for (const auto entry : entries) {
            if (!first)
                out += ',';
            first = false;
            if (!write_string (entry->first, out, error))
                return false;
            out += ':';
            if (!write_value (entry->second, out, error))
                return false;
        }
        out += '}';
    } else {
        using layout = dto_layout<T>;
        out += '{';
        const auto write_field = [&]<std::size_t Position> () {
            constexpr auto field = std::get<layout::order[Position]> (layout::fields);
            if constexpr (Position != 0)
                out += ',';
            if (!write_string (field.name, out, error))
                return false;
            out += ':';
            return write_value (value.*(field.member), out, error);
        };
        if (![&]<std::size_t... I> (std::index_sequence<I...>) {
                return (write_field.template operator()<I> () && ...);
            }(std::make_index_sequence<layout::count>{}))
            return false;
        out += '}';
    }
    return true;
}

enum class sax_kind
{
    null,
    boolean,
    integer,
    unsigned_integer,
    floating,
    string,
    object,
    array
};
struct sax_event
{
    sax_kind kind;
    bool boolean = false;
    std::int64_t integer = 0;
    std::uint64_t unsigned_integer = 0;
    double floating = 0;
    std::string_view text{};
};

class typed_sax final : public nlohmann::json_sax<nlohmann::json>
{
    struct destination
    {
        void *value;
        bool (*accept) (typed_sax &, void *, const sax_event &);
    };
    struct frame
    {
        sax_kind kind;
        void *value;
        destination (*child) (frame &);
        bool (*finish) (typed_sax &, frame &);
        std::vector<std::string> keys;
        bool pending_key = false;
        bool (*key) (frame &, std::string_view) = &remember_key;
        destination next{nullptr, &skip_value};
        std::uint64_t seen = 0;
        std::vector<std::uint64_t> extra_seen;

        std::uint64_t &seen_word (std::size_t index)
        {
            return index == 0 ? seen : extra_seen[index - 1];
        }
    };
    std::vector<frame> frames;
    destination root;
    std::string *error;
    bool started = false;
    bool healthy = true;

    bool reject (const char *message)
    {
        healthy = false;
        return fail (error, message);
    }
    static bool finish_container (typed_sax &, frame &) { return true; }
    static destination skipped_child (frame &) { return {nullptr, &skip_value}; }
    static bool skip_value (typed_sax &in, void *, const sax_event &event)
    {
        if (event.kind == sax_kind::object || event.kind == sax_kind::array)
            in.frames.push_back ({event.kind, nullptr, &skipped_child, &finish_container, {}});
        return true;
    }
    template <typename T> static destination target (T &value) { return {&value, &accept<T>}; }
    template <typename T>
    static bool append_boolean (typed_sax &in, void *pointer, const sax_event &event)
    {
        bool value{};
        if (!accept<bool> (in, &value, event))
            return false;
        static_cast<T *> (pointer)->push_back (value);
        return true;
    }
    template <typename T> static destination child (frame &parent)
    {
        auto &value = *static_cast<T *> (parent.value);
        if constexpr (std::is_same_v<T, nlohmann::json>) {
            if (parent.kind == sax_kind::object)
                return target (value[parent.keys.back ()]);
            value.emplace_back ();
            return target (value.back ());
        } else if constexpr (vector_type<T>::value) {
            if constexpr (std::is_same_v<typename vector_type<T>::value_type, bool>)
                return {&value, &append_boolean<T>};
            else {
                value.emplace_back ();
                return target (value.back ());
            }
        } else if constexpr (map_type<T>::value)
            return target (value[parent.keys.back ()]);
        else {
            return parent.next;
        }
    }
    static bool remember_key (frame &parent, std::string_view key)
    {
        if (std::find (parent.keys.begin (), parent.keys.end (), key) != parent.keys.end ())
            return false;
        parent.keys.emplace_back (key);
        return true;
    }
    template <typename T> static bool dto_key (frame &parent, std::string_view key)
    {
        using layout = dto_layout<T>;
        const auto index = layout::find (key);
        parent.next = {nullptr, &skip_value};
        if (index == layout::count)
            return remember_key (parent, key);
        auto &word = parent.seen_word (index / field_mask_bits);
        const auto bit = std::uint64_t{1} << (index % field_mask_bits);
        if (word & bit)
            return false;
        word |= bit;
        using select = destination (*) (T &);
        static constexpr auto targets = []<std::size_t... I> (std::index_sequence<I...>) {
            return std::array<select, layout::count>{
              +[] (T &value) { return target (value.*(std::get<I> (layout::fields).member)); }...};
        }(std::make_index_sequence<layout::count>{});
        parent.next = targets[index](*static_cast<T *> (parent.value));
        return true;
    }
    template <typename T> static bool finish (typed_sax &in, frame &parent)
    {
        if constexpr (has_dto<T>) {
            constexpr auto count = dto_layout<T>::count;
            for (std::size_t index = 0; index < count; index += field_mask_bits) {
                const auto remaining = count - index;
                const auto required = remaining >= field_mask_bits
                                        ? ~std::uint64_t{0}
                                        : (std::uint64_t{1} << remaining) - 1;
                if (parent.seen_word (index / field_mask_bits) != required)
                    return in.reject ("missing required property");
            }
        }
        return true;
    }
    template <typename T> bool container (T &out, const sax_event &event, sax_kind expected)
    {
        if (event.kind != expected)
            return reject ("unexpected JSON container");
        frame next{expected, &out, &child<T>, &finish<T>, {}};
        if constexpr (has_dto<T>) {
            next.key = &dto_key<T>;
            if constexpr (dto_layout<T>::count > field_mask_bits)
                next.extra_seen.resize ((dto_layout<T>::count - 1) / field_mask_bits);
        }
        frames.push_back (std::move (next));
        return true;
    }
    template <typename T> static bool accept (typed_sax &in, void *pointer, const sax_event &event)
    {
        static_assert (validate_type<T> ());
        auto &out = *static_cast<T *> (pointer);
        if constexpr (std::is_same_v<T, nlohmann::json>) {
            switch (event.kind) {
                case sax_kind::object:
                    out = nlohmann::json::object ();
                    return in.container (out, event, sax_kind::object);
                case sax_kind::array:
                    out = nlohmann::json::array ();
                    return in.container (out, event, sax_kind::array);
                case sax_kind::null:
                    out = nullptr;
                    break;
                case sax_kind::boolean:
                    out = event.boolean;
                    break;
                case sax_kind::integer:
                    out = event.integer;
                    break;
                case sax_kind::unsigned_integer:
                    out = event.unsigned_integer;
                    break;
                case sax_kind::floating:
                    out = event.floating;
                    break;
                case sax_kind::string:
                    out = event.text;
                    break;
            }
        } else if constexpr (optional_type<T>::value) {
            if (event.kind == sax_kind::null)
                out.reset ();
            else {
                out.emplace ();
                return accept<typename optional_type<T>::value_type> (in, &*out, event);
            }
        } else if constexpr (std::is_same_v<T, std::string>) {
            if (event.kind != sax_kind::string)
                return in.reject ("expected string");
            out = event.text;
        } else if constexpr (std::is_same_v<T, bool>) {
            if (event.kind != sax_kind::boolean)
                return in.reject ("expected boolean");
            out = event.boolean;
        } else if constexpr (std::is_integral_v<T>) {
            if constexpr (sizeof (T) == 8) {
                if (event.kind != sax_kind::string)
                    return in.reject ("expected 64-bit decimal string");
                return read_decimal (event.text, out, in.error);
            } else {
                if (event.kind == sax_kind::unsigned_integer) {
                    if (event.unsigned_integer
                        > static_cast<std::uint64_t> (std::numeric_limits<T>::max ()))
                        return in.reject ("integer out of range");
                    out = static_cast<T> (event.unsigned_integer);
                } else if (event.kind == sax_kind::integer) {
                    if (event.integer < static_cast<std::int64_t> (std::numeric_limits<T>::min ())
                        || event.integer
                             > static_cast<std::int64_t> (std::numeric_limits<T>::max ()))
                        return in.reject ("integer out of range");
                    out = static_cast<T> (event.integer);
                } else
                    return in.reject ("expected integer number");
            }
        } else if constexpr (std::is_floating_point_v<T>) {
            double value{};
            if (event.kind == sax_kind::integer)
                value = static_cast<double> (event.integer);
            else if (event.kind == sax_kind::unsigned_integer)
                value = static_cast<double> (event.unsigned_integer);
            else if (event.kind == sax_kind::floating)
                value = event.floating;
            else
                return in.reject ("expected finite number");
            if (!valid_float<T> (value))
                return in.reject ("floating point out of range");
            out = static_cast<T> (value);
        } else if constexpr (std::is_enum_v<T>) {
            if (event.kind != sax_kind::string)
                return in.reject ("expected enum name string");
            return visit_enum_entry<T> ([&] (auto entry) { return entry.name == event.text; },
                                        [&] (auto entry) { out = entry.value; })
                   || in.reject ("undeclared enum name");
        } else if constexpr (std::is_same_v<T, std::vector<std::byte>>) {
            return (event.kind == sax_kind::string && decode_bytes (event.text, out))
                   || in.reject ("invalid base64");
        } else if constexpr (vector_type<T>::value) {
            out.clear ();
            return in.container (out, event, sax_kind::array);
        } else if constexpr (map_type<T>::value) {
            out.clear ();
            return in.container (out, event, sax_kind::object);
        } else
            return in.container (out, event, sax_kind::object);
        return true;
    }
    bool dispatch (const sax_event &event)
    {
        if (!healthy)
            return false;
        destination next = root;
        if (frames.empty ()) {
            if (started)
                return reject ("trailing JSON value");
            started = true;
        } else {
            auto &parent = frames.back ();
            if (parent.kind == sax_kind::object) {
                if (!parent.pending_key)
                    return reject ("missing property name");
                parent.pending_key = false;
            }
            next = parent.child (parent);
        }
        return next.accept (*this, next.value, event);
    }
    bool end (sax_kind kind)
    {
        if (frames.empty () || frames.back ().kind != kind)
            return reject ("unexpected container end");
        if (!frames.back ().finish (*this, frames.back ()))
            return false;
        frames.pop_back ();
        return true;
    }

  public:
    template <typename T>
    typed_sax (T &out, std::string *failure) : root (target (out)), error (failure)
    {
        frames.reserve (initial_frame_capacity);
    }
    bool complete () const { return healthy && started && frames.empty (); }
    bool null () override { return dispatch ({sax_kind::null}); }
    bool boolean (bool value) override
    {
        sax_event e{sax_kind::boolean};
        e.boolean = value;
        return dispatch (e);
    }
    bool number_integer (number_integer_t value) override
    {
        sax_event e{sax_kind::integer};
        e.integer = value;
        return dispatch (e);
    }
    bool number_unsigned (number_unsigned_t value) override
    {
        sax_event e{sax_kind::unsigned_integer};
        e.unsigned_integer = value;
        return dispatch (e);
    }
    bool number_float (number_float_t value, const string_t &) override
    {
        if (!valid_float (value))
            return reject ("non-finite number");
        sax_event e{sax_kind::floating};
        e.floating = value;
        return dispatch (e);
    }
    bool string (string_t &value) override
    {
        sax_event e{sax_kind::string};
        e.text = value;
        return dispatch (e);
    }
    bool binary (binary_t &) override { return reject ("expected JSON value"); }
    bool start_object (std::size_t) override { return dispatch ({sax_kind::object}); }
    bool key (string_t &value) override
    {
        if (frames.empty () || frames.back ().kind != sax_kind::object)
            return reject ("unexpected property name");
        auto &parent = frames.back ();
        if (!parent.key (parent, value))
            return reject ("framework-json-v1 rejects duplicate properties");
        parent.pending_key = true;
        return true;
    }
    bool end_object () override { return end (sax_kind::object); }
    bool start_array (std::size_t) override { return dispatch ({sax_kind::array}); }
    bool end_array () override { return end (sax_kind::array); }
    bool parse_error (std::size_t, const std::string &, const nlohmann::json::exception &) override
    {
        return reject ("invalid JSON");
    }
};
template <typename T>
std::optional<std::string> try_encode (const T &value, std::string *error = nullptr)
{
    std::string out;
    out.reserve (initial_output_capacity);
    if (!write_value (value, out, error))
        return std::nullopt;
    return out;
}
template <typename T, typename Iterator>
std::optional<T> try_decode (Iterator begin, Iterator end, std::string *error)
{
    auto cursor = begin;
    unsigned char prefix[3]{};
    std::size_t size = 0;
    while (size != 3 && cursor != end)
        prefix[size++] = static_cast<unsigned char> (*cursor++);
    if (size == 3 && prefix[0] == 0xef && prefix[1] == 0xbb && prefix[2] == 0xbf) {
        fail (error, "framework-json-v1 rejects a UTF-8 BOM");
        return std::nullopt;
    }
    T value{};
    typed_sax handler (value, error);
    if (!nlohmann::json::sax_parse (begin, end, &handler) || !handler.complete ())
        return std::nullopt;
    return value;
}

template <typename T>
bool to_value (const T &value, nlohmann::json &out, std::string *error = nullptr)
{
    auto text = try_encode (value, error);
    if (!text)
        return false;
    auto parsed = try_parse (text->begin (), text->end (), error);
    if (!parsed)
        return false;
    out = std::move (*parsed);
    return true;
}
template <typename T>
bool from_value (const nlohmann::json &in, T &out, std::string *error = nullptr)
{
    if (!has_finite_numbers (in) || !has_valid_strings (in))
        return fail (error, "invalid JSON value");
    const auto text = dump (in);
    auto value = try_decode<T> (text.begin (), text.end (), error);
    if (!value)
        return false;
    out = std::move (*value);
    return true;
}
#if ZLINK_HAS_EXCEPTIONS
template <typename T> nlohmann::json encode_value (const T &value)
{
    nlohmann::json out;
    std::string error;
    if (!to_value (value, out, &error))
        throw std::invalid_argument (error);
    return out;
}
template <typename T> T decode_value (const nlohmann::json &in)
{
    T out{};
    std::string error;
    if (!from_value (in, out, &error))
        throw std::invalid_argument (error);
    return out;
}
#endif
#if ZLINK_HAS_EXCEPTIONS
template <typename T> std::string encode (const T &value)
{
    std::string error;
    auto result = try_encode (value, &error);
    if (!result)
        throw std::invalid_argument (error);
    return std::move (*result);
}
template <typename T, typename Iterator> T decode (Iterator begin, Iterator end)
{
    std::string error;
    auto result = try_decode<T> (begin, end, &error);
    if (!result)
        throw std::invalid_argument (error);
    return std::move (*result);
}
#endif
} // namespace zlink::detail::json_profile
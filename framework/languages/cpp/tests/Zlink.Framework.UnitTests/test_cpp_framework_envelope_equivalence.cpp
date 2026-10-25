/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "runtime/messaging/envelope_codec.hpp"
#include <nlohmann/detail/exceptions.hpp>

#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef ZLINK_ENVELOPE_REFERENCE_GENERATOR
#include "envelope_decode_reference.hpp"
#endif

namespace
{
namespace fw = zlink::framework;
namespace msg = fw::runtime::messaging;
constexpr std::string_view base =
  R"("formatMarker":242,"kind":3,"channelName":"c","messageName":"m","contentType":"x/y")";

std::string header (std::string_view extra = {})
{
    return "{" + std::string (base) + std::string (extra) + "}";
}

// Keep input generation independent of the decoder: both reference generation
// and the regression executable traverse this exact, deterministic sequence.
std::vector<std::string> corpus ()
{
    std::vector<std::string> cases{
      header (),
      "",
      "null",
      "true",
      "3",
      "[]",
      "{}",
      "[{}]",
      "{{}}",
      header () + " ",
      "\xef\xbb\xbf" + header (),
      header () + "null",
      header () + std::string (1, '\0'),
      header () + std::string (1, '\0') + "garbage",
      header () + " \t" + std::string (1, '\0') + "{broken",
      std::string (1, '\0') + header (),
      " \t\n" + std::string (1, '\0') + header (),
      header (R"(,"metadata":{"a":1,"a":"restored"})"),
      header (R"(,"metadata":{"a":"valid","a":[]})"),
      header (R"(,"metadata":{"a":{},"a":"restored"})"),
      header (R"(,"metadata":{"":1,"":"restored"})"),
      header (R"(,"metadata":{"a\u0000b":"v"})"),
      header (R"(,"metadata":{"a":"v\u0000b"})"),
      header (R"(,"metadata":[],"metadata":{"a":"b"})"),
      header (R"(,"metadata":{"a":"b"},"metadata":null)"),
      header (R"(,"flowId":"01890a5d-ac96-774b-bcce-b302099a8057","flowOrigin":257)"),
      header (R"(,"flowId":"01890a5d-ac96-774b-bcce-b302099a8057","flowOrigin":-255)"),
      header (R"(,"kind":5,"errorCode":"internal_failure")"),
      header (R"(,"kind":5,"kind":false,"errorCode":"internal_failure")"),
      header (R"(,"channelName":"first","channelName":false)"),
      header (R"(,"unknown":{"kind":false,"metadata":{"":"bad"}})"),
      header (R"(,"k\u0069nd":3,"channel\u004eame":"escaped")"),
      header (R"(,"kind\u0000":false,"channelName\u0000":false)"),
      header (R"(,"channelName":"\ud800\udc00\udbff\udfff")"),
      header (R"(,"channelName":"\ud800")"),
      header (R"(,"channelName":"\udc00")"),
      header (R"(,"channelName":"\ud800\u0041")"),
      header (R"(,"channelName":"\x41")"),
      header (R"(,"unknown":"\ud800")"),
      header (R"(,"flowId":"\ud800")"),
      header (R"(,"flowOrigin":[1,])"),
      header (R"(,"unknown":01)"),
      header (R"(,"unknown":1e9999)"),
      header (R"(,"unknown":NaN)"),
      header (R"(,"unknown":-Infinity)")};
    constexpr std::array fields{"formatMarker", "kind",          "channelName", "messageName",
                                "contentType",  "correlationId", "deadline",    "topic",
                                "errorCode",    "errorMessage",  "source",      "flowId",
                                "flowOrigin",   "metadata",      "unknown"};
    const std::vector<std::string> values{"null",
                                          "false",
                                          "true",
                                          "0",
                                          "-0",
                                          "1",
                                          "3",
                                          "5",
                                          "242",
                                          "257",
                                          "-255",
                                          "2147483647",
                                          "2147483648",
                                          "-2147483648",
                                          "-2147483649",
                                          "4294967297",
                                          "9223372036854775807",
                                          "9223372036854775808",
                                          "18446744073709551615",
                                          "18446744073709551616",
                                          "-9223372036854775809",
                                          "3.0",
                                          "3e0",
                                          "1e-9999",
                                          "1e9999",
                                          "0e99999",
                                          "-0e99999",
                                          "0.00000000001e-99999",
                                          "1e-324",
                                          "5e-324",
                                          "1.7976931348623157e308",
                                          "1.7976931348623159e308",
                                          "\"\"",
                                          "\"x\"",
                                          "\"protocol_error\"",
                                          "\"01890a5d-ac96-774b-bcce-b302099a8057\"",
                                          "\"\\u0000\"",
                                          "\"\\b\\f\\n\\r\\t\\/\\\\\\\"\"",
                                          "\"한글😀\"",
                                          "[]",
                                          "{}",
                                          "[null,{\"kind\":3}]",
                                          "{\"a\":\"b\"}",
                                          "{\"a\":1,\"a\":\"b\"}"};
    for (const auto *field : fields) {
        for (std::size_t index = 0; index < values.size (); ++index) {
            const auto member = ",\"" + std::string (field) + "\":";
            cases.push_back (header (member + values[index]));
            cases.push_back (header (member + values[index] + member + "\"last\""));
            cases.push_back (header (member + "\"first\"" + member + values[index]));
        }
    }
    for (const auto *bytes : {"\x80", "\xc0\x80", "\xc2", "\xe0\x80\x80", "\xed\xa0\x80",
                              "\xf4\x90\x80\x80", "\xf5\x80\x80\x80", "\xc2\x80", "\xe0\xa0\x80",
                              "\xed\x9f\xbf", "\xf0\x90\x80\x80", "\xf4\x8f\xbf\xbf"}) {
        for (const auto *field : {"channelName", "unknown", "flowId"})
            cases.push_back (header (",\"" + std::string (field) + "\":\"" + bytes + "\""));
    }
    for (const std::size_t size : {15u, 16u, 31u, 32u, 64u, 65u, 1015u, 1016u, 1017u, 1024u}) {
        cases.push_back (header (",\"correlationId\":\"" + std::string (size, 'c') + "\""));
        cases.push_back (header (",\"metadata\":{\"a\":\"" + std::string (size, 'v') + "\"}"));
        cases.push_back (header (",\"unknown\":\"" + std::string (size, 'u') + "\""));
    }
    for (const auto *escape : {"\\u0001", "\\\"", "\\\\"}) {
        for (const std::size_t bytes : {1023u, 1024u, 1025u}) {
            const auto width = std::string_view (escape) == "\\u0001" ? 6u : 2u;
            const auto repetitions = (bytes - 8) / width;
            std::string value;
            for (std::size_t index = 0; index < repetitions; ++index)
                value += escape;
            value += std::string (bytes - 8 - repetitions * width, 'x');
            cases.push_back (header (",\"metadata\":{\"a\":\"" + value + "\"}"));
        }
    }
    for (const std::size_t depth : {1u, 8u, 64u, 256u, 1024u, 4096u}) {
        cases.push_back (
          header (",\"unknown\":" + std::string (depth, '[') + "0" + std::string (depth, ']')));
        cases.push_back (
          header (",\"flowId\":" + std::string (depth, '[') + "0" + std::string (depth, ']')));
        std::string nested;
        for (std::size_t index = 0; index < depth; ++index)
            nested += "{\"x\":";
        cases.push_back (header (",\"unknown\":" + nested + "0" + std::string (depth, '}')));
    }
    for (unsigned value = 0; value < 32; ++value)
        cases.push_back (
          header (",\"unknown\":\"" + std::string (1, static_cast<char> (value)) + "\""));
    for (const auto *bom : {"\xef", "\xef\xbb", "\xef\xbb\xbf"}) {
        cases.push_back (std::string (bom) + header ());
        cases.push_back (header (",\"unknown\":" + std::string (bom) + "0"));
        cases.push_back (header (",\"unknown\":\"" + std::string (bom) + "\""));
    }
    cases.push_back (header (R"(,"unknown":{"x":1,})"));
    cases.push_back (header (R"(,"unknown":[1,])"));
    const auto complete = header (R"(,"metadata":{"a":"b"},"flowId":null,"flowOrigin":null)");
    for (std::size_t size = 0; size < complete.size (); ++size)
        cases.push_back (complete.substr (0, size));
    for (std::size_t position = 0; position <= complete.size (); ++position) {
        auto inserted = complete;
        inserted.insert (position, 1, '\0');
        cases.push_back (std::move (inserted));
        cases.push_back (complete.substr (0, position) + std::string (1, '\0') + "\xc0\x80");
    }
    std::uint32_t random = 0x1639b2u;
    for (unsigned index = 0; index < 512; ++index) {
        random = random * 1664525u + 1013904223u;
        auto mutated = complete;
        const auto position = random % mutated.size ();
        random = random * 1664525u + 1013904223u;
        constexpr std::string_view replacements = "{}[],:\\\"0129-+eEntfru \t\r\n";
        mutated[position] = replacements[random % replacements.size ()];
        cases.push_back (std::move (mutated));
    }
    return cases;
}

class observation_t
{
  public:
    void number (std::uint64_t value)
    {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            _hash ^= static_cast<std::uint8_t> (value >> shift);
            _hash *= 1099511628211ull;
        }
    }
    void text (std::string_view value)
    {
        number (value.size ());
        for (const unsigned char byte : value) {
            _hash ^= byte;
            _hash *= 1099511628211ull;
        }
    }
    void optional (const std::optional<std::string> &value)
    {
        number (value.has_value ());
        if (value)
            text (*value);
    }
    void header (const msg::envelope_header_t &value)
    {
        number (static_cast<std::uint64_t> (value.kind));
        text (value.channel_name);
        text (value.message_name);
        text (value.content_type);
        text (value.correlation_id);
        optional (value.deadline);
        optional (value.topic);
        optional (value.error_code);
        optional (value.error_message);
        optional (value.source);
        number (value.metadata.size ());
        for (const auto &[key, item] : value.metadata) {
            text (key);
            text (item);
        }
        optional (value.flow_id);
        number (value.flow_origin.has_value ());
        if (value.flow_origin)
            number (static_cast<std::uint64_t> (*value.flow_origin));
    }
    std::uint64_t value () const { return _hash; }

  private:
    std::uint64_t _hash = 14695981039346656037ull;
};

std::uint64_t observe (const std::string &wire, bool capture_flow)
{
    // A preexisting value must be reset even if JSON parsing fails.
    std::optional<msg::envelope_header_t> rejected{msg::envelope_header_t{}};
    rejected->channel_name = "stale rejection context";
    const auto result = msg::envelope_codec_t{}.decode_header (zlink::message_t::from (wire),
                                                               capture_flow, &rejected);
    observation_t output;
    output.number (static_cast<bool> (result));
    if (result) {
        output.header (result.value ());
        output.text (msg::envelope_codec_t{}.encode_header (result.value ()).to_string ());
    } else
        output.number (static_cast<std::uint64_t> (result.error_kind ()));
    output.number (rejected.has_value ());
    if (rejected)
        output.header (*rejected);
    return output.value ();
}

std::vector<msg::envelope_header_t> writer_corpus ()
{
    std::vector<std::string> texts{"", "plain", "한글😀", std::string (2048, 'x')};
    for (unsigned byte = 0; byte < 256; ++byte) {
        texts.emplace_back (1, static_cast<char> (byte));
        texts.push_back ("prefix" + texts.back ());
    }
    const std::vector<std::string> sequences{
      "\xc0\x80",         "\xc1\xbf",         "\xc2\x80",         "\xdf\xbf",
      "\xe0\xa0\x80",     "\xe0\x9f\xbf",     "\xed\x9f\xbf",     "\xed\xa0\x80",
      "\xef\xbf\xbf",     "\xf0\x8f\xbf\xbf", "\xf0\x90\x80\x80", "\xf4\x8f\xbf\xbf",
      "\xf4\x90\x80\x80", "\xf5\x80\x80\x80", "\xff\xff\xff\xff"};
    for (const auto &sequence : sequences) {
        for (std::size_t size = 1; size <= sequence.size (); ++size) {
            texts.push_back (sequence.substr (0, size));
            texts.push_back ("prefix" + sequence.substr (0, size));
            texts.push_back (sequence.substr (0, size) + "suffix");
        }
        for (std::size_t position = 1; position < sequence.size (); ++position) {
            auto wrong_continuation = sequence;
            wrong_continuation[position] = 'X';
            texts.push_back (std::move (wrong_continuation));
        }
    }
    std::string ascii;
    for (unsigned byte = 0; byte < 128; ++byte)
        ascii.push_back (static_cast<char> (byte));
    texts.push_back (std::move (ascii));
    std::vector<msg::envelope_header_t> headers;
    for (const auto &text : texts) {
        msg::envelope_header_t value{
          .channel_name = "c", .message_name = "m", .content_type = "x/y"};
        value.channel_name = text;
        headers.push_back (value);
        value.channel_name = "c";
        value.error_message = text;
        headers.push_back (value);
        value.error_message.reset ();
        value.metadata.emplace ("a", text);
        headers.push_back (value);
        value.metadata.clear ();
        value.metadata.emplace (text, "v");
        headers.push_back (std::move (value));
    }
    for (const auto escape : {'x', '\x01', '"', '\\'}) {
        for (const std::size_t bytes : {1023u, 1024u, 1025u}) {
            const auto width = escape == '\x01' ? 6u : escape == 'x' ? 1u : 2u;
            std::string text ((bytes - 8) / width, escape);
            text += std::string ((bytes - 8) % width, 'x');
            msg::envelope_header_t value{
              .channel_name = "c", .message_name = "m", .content_type = "x/y"};
            value.metadata.emplace ("a", std::move (text));
            headers.push_back (std::move (value));
        }
    }
    return headers;
}

std::uint64_t observe_writer (const msg::envelope_header_t &header,
                              std::string *diagnostic = nullptr)
{
    observation_t output;
    try {
        const auto wire = msg::envelope_codec_t{}.encode_header (header);
        output.number (0);
        output.text (wire.to_string ());
    }
    catch (const nlohmann::detail::type_error &error) {
        if (diagnostic)
            *diagnostic = error.what ();
        output.number (1);
        output.number (error.id);
        output.text (error.what ());
    }
    catch (const fw::framework_exception_t &error) {
        if (diagnostic)
            *diagnostic = error.what ();
        output.number (2);
        output.number (static_cast<std::uint64_t> (error.kind ()));
        output.text (error.what ());
    }
    catch (const std::exception &error) {
        if (diagnostic)
            *diagnostic = error.what ();
        output.number (3);
        output.text (error.what ());
    }
    return output.value ();
}

void require (bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error (message);
}

void verify_readable_samples ()
{
    msg::envelope_codec_t codec;
    const auto restored =
      codec.decode_header (zlink::message_t::from (header (R"(,"metadata":{"a":1,"a":"last"})")));
    require (restored && restored.value ().metadata.at ("a") == "last",
             "last duplicate metadata value must restore an invalid prior value");
    std::optional<msg::envelope_header_t> rejected;
    const auto bad_kind = codec.decode_header (
      zlink::message_t::from (header (R"(,"kind":5,"kind":false)")), true, &rejected);
    require (!bad_kind && rejected && rejected->kind == msg::message_kind_t::error,
             "rejection context must preserve the prior numeric kind after an invalid duplicate");
    const auto ignored =
      zlink::message_t::from (header (R"(,"flowId":{"ignored":[1,true]},"flowOrigin":[0])"));
    const auto flow_off = codec.decode_header (ignored, false);
    require (flow_off && !flow_off.value ().flow_id && !flow_off.value ().flow_origin
               && !codec.decode_header (ignored, true),
             "flow Off must ignore observation meaning while flow capture validates it");
    const auto syntax = zlink::message_t::from (header (R"(,"flowId":[1,])"));
    require (!codec.decode_header (syntax, false), "flow Off must still validate JSON syntax");
    const auto utf8 = zlink::message_t::from (header (",\"flowId\":\"\xc0\x80\""));
    require (!codec.decode_header (utf8, false), "flow Off must still validate UTF-8");
}
} // namespace

int main (int argc, char **argv)
{
    try {
        verify_readable_samples ();
        const auto cases = corpus ();
        const auto writer_cases = writer_corpus ();
        observation_t corpus_identity;
        for (const auto &wire : cases)
            corpus_identity.text (wire);
        for (const auto &header : writer_cases)
            corpus_identity.header (header);
        constexpr std::size_t block_observations = 64;
        std::vector<std::uint64_t> decoder_blocks;
        std::vector<std::uint64_t> writer_blocks;
        for (std::size_t begin = 0; begin < cases.size () * 2; begin += block_observations) {
            observation_t block;
            for (std::size_t index = begin;
                 index < cases.size () * 2 && index < begin + block_observations; ++index)
                block.number (observe (cases[index / 2], index % 2 != 0));
            decoder_blocks.push_back (block.value ());
        }
        for (std::size_t begin = 0; begin < writer_cases.size (); begin += block_observations) {
            observation_t block;
            for (std::size_t index = begin;
                 index < writer_cases.size () && index < begin + block_observations; ++index)
                block.number (observe_writer (writer_cases[index]));
            writer_blocks.push_back (block.value ());
        }
#ifdef ZLINK_ENVELOPE_REFERENCE_GENERATOR
        require (argc == 2, "reference generator requires an output path");
        std::ofstream output (argv[1]);
        require (static_cast<bool> (output), "cannot open reference output");
        output << "/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */\n#pragma once\n"
                  "// 64-observation blocks from the actual codec at 73b0d9e514.\n"
                  "// Regenerate only with that codec and ZLINK_ENVELOPE_REFERENCE_GENERATOR.\n"
                  "inline constexpr std::uint64_t envelope_corpus_identity = "
               << corpus_identity.value ()
               << "ull;\n"
                  "inline constexpr std::size_t envelope_decoder_observations = "
               << cases.size () * 2
               << ";\n"
                  "inline constexpr std::size_t envelope_writer_observations = "
               << writer_cases.size () << ";\n";
        const auto write_blocks = [&] (const char *name, const auto &values) {
            output << "inline constexpr std::uint64_t " << name << "[] = {\n";
            for (const auto value : values)
                output << "  " << value << "ull,\n";
            output << "};\n";
        };
        write_blocks ("envelope_decode_reference", decoder_blocks);
        write_blocks ("envelope_encode_reference", writer_blocks);
#else
        (void) argc;
        (void) argv;
        require (corpus_identity.value () == envelope_corpus_identity,
                 "corpus input changed; regenerate with the original parser and writer");
        require (envelope_decoder_observations == cases.size () * 2
                   && std::size (envelope_decode_reference) == decoder_blocks.size (),
                 "reference corpus size changed; regenerate with the original parser");
        require (envelope_writer_observations == writer_cases.size ()
                   && std::size (envelope_encode_reference) == writer_blocks.size (),
                 "writer reference corpus size changed; regenerate with the original writer");
        unsigned failures = 0;
        for (std::size_t block = 0; block < decoder_blocks.size (); ++block) {
            if (decoder_blocks[block] == envelope_decode_reference[block])
                continue;
            ++failures;
            const auto begin = block * block_observations / 2;
            const auto end = std::min (cases.size (), begin + block_observations / 2);
            std::cerr << "decoder block=" << block << " input range=[" << begin << ',' << end
                      << ") expected=" << envelope_decode_reference[block]
                      << " actual=" << decoder_blocks[block] << '\n';
            for (std::size_t index = begin; index < end; ++index)
                std::cerr << "input=" << index << " wire=" << cases[index] << '\n';
        }
        for (std::size_t block = 0; block < writer_blocks.size (); ++block) {
            if (writer_blocks[block] == envelope_encode_reference[block])
                continue;
            ++failures;
            const auto begin = block * block_observations;
            const auto end = std::min (writer_cases.size (), begin + block_observations);
            std::cerr << "writer block=" << block << " input range=[" << begin << ',' << end
                      << ") expected=" << envelope_encode_reference[block]
                      << " actual=" << writer_blocks[block] << '\n';
            for (std::size_t index = begin; index < end; ++index) {
                std::string diagnostic;
                (void) observe_writer (writer_cases[index], &diagnostic);
                std::cerr << "input=" << index << " channel=" << writer_cases[index].channel_name
                          << " diagnostic=" << diagnostic << '\n';
            }
        }
        require (failures == 0, "envelope codec differs from the original codec observations");
#endif
        std::cout << "Envelope equivalence: " << cases.size () << " inputs, " << cases.size () * 2
                  << " decoder observations and " << writer_cases.size ()
                  << " writer observations passed\n";
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << error.what () << '\n';
        return 1;
    }
}

/* SPDX-License-Identifier: Elastic-2.0 */
#include <zlink/detail/json_profile/engine.hpp>

#include <yyjson.h>

#include <memory>

namespace zlink::detail::json_profile
{
namespace
{

bool flatten (yyjson_val *value, token_buffer &output, std::string_view key = {})
{
    const auto parent_index = output.tokens.size ();
    output.tokens.emplace_back ();
    auto &entry = output.tokens[parent_index];
    if (!key.empty ()) {
        entry.key_offset = output.strings.size ();
        entry.key_length = key.size ();
        output.strings.append (key);
    }
    if (yyjson_is_obj (value)) {
        entry.kind = token_kind::object;
        std::size_t item_index = 0, count = 0;
        yyjson_val *property = nullptr, *child = nullptr;
        yyjson_obj_foreach (value, item_index, count, property, child)
        {
            const std::string_view name (yyjson_get_str (property), yyjson_get_len (property));
            if (!flatten (child, output, name))
                return false;
            ++output.tokens[parent_index].child_count;
        }
        return true;
    }
    if (yyjson_is_arr (value)) {
        entry.kind = token_kind::array;
        std::size_t item_index = 0, count = 0;
        yyjson_val *child = nullptr;
        yyjson_arr_foreach (value, item_index, count, child)
        {
            if (!flatten (child, output))
                return false;
            ++output.tokens[parent_index].child_count;
        }
        return true;
    }
    if (yyjson_is_str (value)) {
        entry.kind = token_kind::string;
        entry.text_offset = output.strings.size ();
        entry.text_length = yyjson_get_len (value);
        output.strings.append (yyjson_get_str (value), entry.text_length);
    } else if (yyjson_is_uint (value)) {
        entry.kind = token_kind::unsigned_integer;
        entry.unsigned_integer = yyjson_get_uint (value);
    } else if (yyjson_is_sint (value)) {
        entry.kind = token_kind::integer;
        entry.integer = yyjson_get_sint (value);
    } else if (yyjson_is_real (value)) {
        entry.kind = token_kind::floating;
        entry.floating = yyjson_get_real (value);
    } else if (yyjson_is_bool (value)) {
        entry.kind = token_kind::boolean;
        entry.boolean = yyjson_get_bool (value);
    } else {
        entry.kind = token_kind::null;
    }
    return true;
}

} // namespace

namespace ZLINK_JSON_PROFILE_ENGINE_NS
{
#if defined(__GNUC__) || defined(__clang__)
__attribute__ ((visibility ("default")))
#endif
bool parse_tokens (std::string_view input, token_buffer &output)
{
    output.tokens.clear ();
    output.strings.clear ();
    const std::unique_ptr<yyjson_doc, decltype (&yyjson_doc_free)> document (
      yyjson_read (input.data (), input.size (), 0), &yyjson_doc_free);
    return document && flatten (yyjson_doc_get_root (document.get ()), output);
}
} // namespace ZLINK_JSON_PROFILE_ENGINE_NS

} // namespace zlink::detail::json_profile

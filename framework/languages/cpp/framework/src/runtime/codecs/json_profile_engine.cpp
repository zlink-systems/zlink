/* SPDX-License-Identifier: Elastic-2.0 */
#include <zlink/detail/json_profile/engine.hpp>

#include <yyjson.h>

#include <memory>
#include <vector>

namespace zlink::detail::json_profile
{
namespace
{

// yyjson reads without recursion; flattening keeps that property with an
// explicit stack so nesting depth never grows the call stack.
struct open_container
{
    std::size_t token_index;
    bool object;
    yyjson_obj_iter properties;
    yyjson_arr_iter items;
};

thread_local std::vector<open_container> open_containers;

void append_token (yyjson_val *value, std::string_view key, token_buffer &output)
{
    const auto index = output.tokens.size ();
    auto &entry = output.tokens.emplace_back ();
    if (!key.empty ()) {
        entry.key_offset = output.strings.size ();
        entry.key_length = key.size ();
        output.strings.append (key);
    }
    if (yyjson_is_obj (value)) {
        entry.kind = token_kind::object;
        auto &container = open_containers.emplace_back ();
        container.token_index = index;
        container.object = true;
        yyjson_obj_iter_init (value, &container.properties);
    } else if (yyjson_is_arr (value)) {
        entry.kind = token_kind::array;
        auto &container = open_containers.emplace_back ();
        container.token_index = index;
        container.object = false;
        yyjson_arr_iter_init (value, &container.items);
    } else if (yyjson_is_str (value)) {
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
}

void flatten (yyjson_val *root, token_buffer &output)
{
    open_containers.clear ();
    append_token (root, {}, output);
    while (!open_containers.empty ()) {
        auto &container = open_containers.back ();
        const auto parent = container.token_index;
        yyjson_val *child = nullptr;
        std::string_view key;
        if (container.object) {
            yyjson_val *name = yyjson_obj_iter_next (&container.properties);
            if (name) {
                key = {yyjson_get_str (name), yyjson_get_len (name)};
                child = yyjson_obj_iter_get_val (name);
            }
        } else {
            child = yyjson_arr_iter_next (&container.items);
        }
        if (!child) {
            open_containers.pop_back ();
            continue;
        }
        ++output.tokens[parent].child_count;
        append_token (child, key, output);
    }
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
    if (!document)
        return false;
    flatten (yyjson_doc_get_root (document.get ()), output);
    return true;
}
} // namespace ZLINK_JSON_PROFILE_ENGINE_NS

} // namespace zlink::detail::json_profile

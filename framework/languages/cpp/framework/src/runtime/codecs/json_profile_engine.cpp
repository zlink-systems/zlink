/* SPDX-License-Identifier: Elastic-2.0 */
#include <zlink/json_profile.hpp>

#include <yyjson.h>

#include <array>
#include <memory>
#include <memory_resource>
#include <vector>

namespace zlink::detail::json_profile
{
namespace
{

struct open_container
{
    bool object;
    yyjson_obj_iter properties{};
    yyjson_arr_iter items{};
};

sax_event read_value (yyjson_val *value)
{
    switch (yyjson_get_tag (value)) {
        case YYJSON_TYPE_OBJ:
            return {sax_kind::object};
        case YYJSON_TYPE_ARR:
            return {sax_kind::array};
        case YYJSON_TYPE_STR:
        case YYJSON_TYPE_STR | YYJSON_SUBTYPE_NOESC:
            return {
              sax_kind::string, false, 0, 0, 0, {yyjson_get_str (value), yyjson_get_len (value)}};
        case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_UINT:
            return {sax_kind::unsigned_integer, false, 0, yyjson_get_uint (value)};
        case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_SINT:
            return {sax_kind::integer, false, yyjson_get_sint (value)};
        case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_REAL:
            return {sax_kind::floating, false, 0, 0, yyjson_get_real (value)};
        case YYJSON_TYPE_BOOL | YYJSON_SUBTYPE_TRUE:
        case YYJSON_TYPE_BOOL | YYJSON_SUBTYPE_FALSE:
            return {sax_kind::boolean, yyjson_get_bool (value)};
        default:
            return {sax_kind::null};
    }
}

template <typename Value, typename Key, typename End>
bool visit_document (std::string_view input, Value value, Key key, End end)
{
    const std::unique_ptr<yyjson_doc, decltype (&yyjson_doc_free)> document (
      yyjson_read (input.data (), input.size (), 0), &yyjson_doc_free);
    if (!document)
        return false;

    // Keep nesting off the call stack and permit nested decodes on the same thread.
    alignas (open_container) std::array<std::byte, 16 * sizeof (open_container)> storage;
    std::pmr::monotonic_buffer_resource resource (storage.data (), storage.size ());
    std::pmr::vector<open_container> open (&resource);
    open.reserve (8);

    auto *current = yyjson_doc_get_root (document.get ());
    for (;;) {
        const auto event = read_value (current);
        if (!value (event))
            return false;

        if (event.kind == sax_kind::object) {
            auto &container = open.emplace_back (open_container{true});
            yyjson_obj_iter_init (current, &container.properties);
        } else if (event.kind == sax_kind::array) {
            auto &container = open.emplace_back (open_container{false});
            yyjson_arr_iter_init (current, &container.items);
        }

        current = nullptr;
        while (!open.empty () && !current) {
            auto &container = open.back ();
            if (container.object) {
                if (auto *name = yyjson_obj_iter_next (&container.properties)) {
                    if (!key ({yyjson_get_str (name), yyjson_get_len (name)}))
                        return false;
                    current = yyjson_obj_iter_get_val (name);
                }
            } else {
                current = yyjson_arr_iter_next (&container.items);
            }
            if (!current) {
                open.pop_back ();
                if (!end ())
                    return false;
            }
        }
        if (!current)
            return true;
    }
}

} // namespace

namespace ZLINK_JSON_PROFILE_ENGINE_NS
{
#if defined(__GNUC__) || defined(__clang__)
__attribute__ ((visibility ("default")))
#endif
bool read_document (std::string_view input, typed_sax &handler)
{
    input = input.substr (0, input.find ('\0'));
    return visit_document (
      input, [&] (const sax_event &event) { return handler.value (event); },
      [&] (std::string_view key) { return handler.borrowed_key (key); },
      [&] { return handler.end_container (); });
}

} // namespace ZLINK_JSON_PROFILE_ENGINE_NS

} // namespace zlink::detail::json_profile

#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <zlink/framework/contracts/codecs/serializer.hpp>

namespace test_json_morpg_lobby_domain
{
template <class T> inline std::string encode (const T &value)
{
    zlink::framework::serializer_registry_t serializers;
    return serializers.get<T> ().serialize (value).to_string ();
}
template <class T> inline nlohmann::json object (const T &value)
{
    return nlohmann::json::parse (encode (value));
}
template <class T> inline T decode (const nlohmann::json &value)
{
    zlink::framework::serializer_registry_t serializers;
    return serializers.get<T> ().deserialize (
      zlink::framework::encoded_payload_t::from_string (value.dump ()));
}
} // namespace test_json_morpg_lobby_domain

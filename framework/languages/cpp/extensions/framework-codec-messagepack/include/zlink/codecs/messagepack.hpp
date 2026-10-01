/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/configuration/framework_options.hpp>

namespace zlink::framework_codecs
{

class messagepack_codec_extension_t
{
  public:
    template <typename TBuilder> void register_framework_codecs (TBuilder &codecs) const
    {
        (void) codecs;
    }

  public:
    template <typename TPayload, typename TBuilder>
    static void register_payload_serializer (TBuilder &codecs)
    {
        codecs.template add_serializer<TPayload> (
          [] (const TPayload &value) {
              return zlink::framework::detail::encoded_payload_from_raw (
                zlink::message_t::from_json (value));
          },
          [] (const zlink::framework::encoded_payload_t &payload) {
              return zlink::framework::detail::encoded_payload_to_raw (payload)
                .template parse_json<TPayload> ();
          },
          "application/x-msgpack");
    }
};

inline messagepack_codec_extension_t messagepack ()
{
    return {};
}

template <typename TPayload, typename... TPayloads>
messagepack_codec_extension_t messagepack () = delete;

} // namespace zlink::framework_codecs

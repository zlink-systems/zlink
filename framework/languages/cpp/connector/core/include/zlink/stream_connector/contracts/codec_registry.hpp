/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/stream_connector/contracts/zlink_stream_enums.hpp>

#include <memory>

namespace zlink::stream_connector
{

class codec_registry_t
{
  public:
    codec_registry_t ();
    ~codec_registry_t ();

    codec_registry_t (codec_registry_t &&) noexcept;
    codec_registry_t &operator= (codec_registry_t &&) noexcept;
    codec_registry_t (const codec_registry_t &) = default;
    codec_registry_t &operator= (const codec_registry_t &) = default;

    codec_registry_t &enable_codec (codec_t codec);
    codec_registry_t &use_default_codec (codec_t codec);
    template <typename TExtension> codec_registry_t &use (const TExtension &extension)
    {
        extension.register_connector_codecs (*this);
        return *this;
    }

    bool supports (codec_t codec) const;

  private:
    friend class connector_t;
    explicit codec_registry_t (std::shared_ptr<void> state);

    std::shared_ptr<void> _state;
};

} // namespace zlink::stream_connector

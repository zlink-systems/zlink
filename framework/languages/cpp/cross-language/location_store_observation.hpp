/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "runtime/locations/provider_location_repository.hpp"

namespace cross_language
{
namespace fw = zlink::framework;

// Observation uses the repository's own key and record codecs. The overlay is
// read-only: candidate writes never escape to Redis before their release.
class location_store_observation_t final : public fw::location_store_t
{
  public:
    explicit location_store_observation_t (fw::location_store_t *inner = nullptr,
                                           const fw::store_write_request_t *overlay = nullptr) :
        _inner (inner), _overlay (overlay)
    {
    }

    location_store_observation_t (fw::store_key_t key, const fw::store_read_result_t &read) :
        _exact_key (std::move (key)), _exact (&read)
    {
    }

    fw::task_t<fw::store_read_result_t> read (fw::store_key_t key) override
    {
        observed_key = key;
        if (_exact && key.value == _exact_key.value)
            co_return *_exact;
        if (_overlay) {
            for (const auto &mutation : _overlay->mutations) {
                if (const auto *put = std::get_if<fw::store_put_t> (&mutation);
                    put && put->key.value == key.value) {
                    co_return fw::store_found_t{
                      fw::store_value_t{put->bytes,
                                        {"observation"},
                                        std::nullopt,
                                        std::chrono::system_clock::now ()}};
                }
                if (const auto *removed = std::get_if<fw::store_delete_t> (&mutation);
                    removed && removed->key.value == key.value)
                    co_return fw::store_missing_t{std::chrono::system_clock::now ()};
            }
        }
        if (_inner)
            co_return co_await _inner->read (std::move (key));
        co_return fw::store_missing_t{std::chrono::system_clock::now ()};
    }

    fw::task_t<fw::store_write_result_t> write (fw::store_write_request_t) override
    {
        throw std::logic_error ("Location Store observation cannot write");
        co_return fw::store_write_conflict_t{};
    }

    fw::task_t<fw::store_scan_result_t> scan (fw::store_scan_request_t request) override
    {
        if (!_inner)
            throw std::logic_error ("key observation cannot scan");
        co_return co_await _inner->scan (std::move (request));
    }

    fw::store_key_t observed_key;

  private:
    fw::location_store_t *_inner = nullptr;
    const fw::store_write_request_t *_overlay = nullptr;
    fw::store_key_t _exact_key;
    const fw::store_read_result_t *_exact = nullptr;
};

} // namespace cross_language

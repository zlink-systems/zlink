/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <runtime/locations/location_repository.hpp>
#include <zlink/framework/contracts/locations/stores.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <random>
#include <sstream>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace zlink::framework::runtime
{

/*
 * Converts the public immutable-blob SPI into the domain repository used by
 * relocation orchestration. Reference generation and checksums remain
 * Framework responsibilities; a provider only stores opaque bytes.
 */
class provider_relocation_repository_t final :
    public relocation_repository_t
{
  public:
    explicit provider_relocation_repository_t (
      relocation_store_t &store) noexcept :
        _store (&store)
    {
    }

    task_t<relocation_stored_t> put_relocation (
      std::vector<std::byte> payload,
      std::chrono::hours retention,
      std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            return cancelled<relocation_stored_t> ();
        if (retention <= std::chrono::hours::zero ())
            return failed<relocation_stored_t> (
              framework_error_kind_t::protocol_error,
              "relocation retention must be positive");

        const auto retention_ms =
          std::chrono::duration_cast<std::chrono::milliseconds> (
            retention);
        const auto checksum = crc32c (payload);
        for (unsigned attempt = 0; attempt != 4; ++attempt) {
            const auto reference = make_reference ();
            for (unsigned retry = 0; retry != 2; ++retry) {
                auto result = _store
                                ->put (
                                  blob_reference_t{reference},
                                  std::span<const std::byte> (
                                    payload.data (), payload.size ()),
                                  retention_ms)
                                .result ();
                if (result) {
                    const auto &written = result.value ();
                    if (const auto *stored =
                          std::get_if<blob_stored_t> (
                            &written))
                        return completed (
                          relocation_stored_t{
                            reference,
                            checksum,
                            stored->expires_at,
                            stored->store_now});
                    if (std::holds_alternative<
                          blob_conflict_t> (written))
                        break;
                }

                auto read =
                  _store
                    ->read (blob_reference_t{reference})
                    .result ();
                if (read) {
                    if (const auto *found =
                          std::get_if<blob_found_t> (
                            &read.value ())) {
                        if (found->bytes != payload)
                            break;
                        return completed (
                          relocation_stored_t{
                            reference,
                            checksum,
                            found->expires_at,
                            found->store_now});
                    }
                }

                if (result
                    || result.error () == nullptr
                    || !detail::is_transient_error (result.error ()->kind ())
                    || retry != 0) {
                    if (!result)
                        return task_t<relocation_stored_t> (
                          detail::propagate_failure<
                            relocation_stored_t> (
                            result,
                            "relocation Store put failed"));
                    if (!read)
                        return task_t<relocation_stored_t> (
                          detail::propagate_failure<
                            relocation_stored_t> (
                            read,
                            "relocation Store read-back failed"));
                }
            }
            // A generated reference collided with different immutable bytes.
            // Retry with a fresh Framework-issued reference.
        }
        return failed<relocation_stored_t> (
          framework_error_kind_t::capacity_exceeded,
          "could not allocate a unique relocation reference");
    }

    task_t<relocation_read_result_t> get_relocation (
      std::string reference,
      std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            return cancelled<relocation_read_result_t> ();
        auto result =
          _store->read (
                  blob_reference_t{std::move (reference)})
            .result ();
        if (!result)
            return task_t<relocation_read_result_t> (
              detail::propagate_failure<
                relocation_read_result_t> (
                result,
                "relocation Store read failed"));
        if (const auto *found =
              std::get_if<blob_found_t> (&result.value ()))
            return completed (
              relocation_read_result_t{
                relocation_found_t{found->bytes}});
        return completed (
          relocation_read_result_t{relocation_missing_t{}});
    }

    task_t<relocation_renew_result_t> renew_relocation (
      std::string reference,
      std::chrono::hours retention,
      std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            return cancelled<relocation_renew_result_t> ();
        if (retention <= std::chrono::hours::zero ())
            return failed<relocation_renew_result_t> (
              framework_error_kind_t::protocol_error,
              "relocation retention must be positive");
        auto result = _store
                        ->renew (
                          blob_reference_t{
                            std::move (reference)},
                          std::chrono::duration_cast<
                            std::chrono::milliseconds> (
                            retention))
                        .result ();
        if (!result)
            return task_t<relocation_renew_result_t> (
              detail::propagate_failure<
                relocation_renew_result_t> (
                result,
                "relocation Store renew failed"));
        if (const auto *renewed =
              std::get_if<blob_renewed_t> (&result.value ()))
            return completed (
              relocation_renew_result_t{
                relocation_renewed_t{
                  renewed->expires_at,
                  renewed->store_now}});
        return completed (
          relocation_renew_result_t{
            relocation_renew_missing_t{}});
    }

    task_t<relocation_delete_result_t> delete_relocation (
      std::string reference,
      std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            return cancelled<relocation_delete_result_t> ();
        auto result =
          _store->erase (
                  blob_reference_t{std::move (reference)})
            .result ();
        if (!result)
            return task_t<relocation_delete_result_t> (
              detail::propagate_failure<
                relocation_delete_result_t> (
                result,
                "relocation Store erase failed"));
        return completed (
          relocation_delete_result_t::deleted);
    }

  private:
    template <typename T>
    static task_t<T> completed (T value)
    {
        return task_t<T> (
          result_t<T>::success (std::move (value)));
    }

    template <typename T>
    static task_t<T> failed (
      framework_error_kind_t kind,
      std::string message)
    {
        return task_t<T> (
          result_t<T>::failure (kind, std::move (message)));
    }

    template <typename T> static task_t<T> cancelled ()
    {
        return task_t<T> (
          detail::boundary_failure<T> (
            detail::boundary_error_t::cancelled,
            "relocation Store operation was cancelled"));
    }

    static std::string make_reference ()
    {
        std::array<std::uint64_t, 2> words{};
        std::random_device random;
        for (auto &word : words)
            word =
              (static_cast<std::uint64_t> (random ()) << 32)
              | static_cast<std::uint64_t> (random ());
        std::ostringstream stream;
        stream << "zlr-" << std::hex << std::setfill ('0');
        for (const auto word : words)
            stream << std::setw (16) << word;
        return stream.str ();
    }

    static std::uint32_t crc32c (
      const std::vector<std::byte> &payload) noexcept
    {
        std::uint32_t crc = 0xFFFFFFFFU;
        for (const auto byte : payload) {
            crc ^= std::to_integer<std::uint8_t> (byte);
            for (int bit = 0; bit < 8; ++bit)
                crc =
                  (crc >> 1)
                  ^ (0x82F63B78U
                     & (0U - (crc & 1U)));
        }
        return ~crc;
    }

    relocation_store_t *_store;
};

} // namespace zlink::framework::runtime

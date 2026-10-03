/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <runtime/locations/location_repository.hpp>
#include <runtime/execution/task_result.hpp>
#include <zlink/framework/contracts/locations/stores.hpp>
#include <zlink/framework/detail/crc32c.hpp>

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
class provider_relocation_repository_t final : public relocation_repository_t
{
  public:
    explicit provider_relocation_repository_t (relocation_store_t &store) noexcept : _store (&store)
    {
    }

    task_t<relocation_stored_t>
    put_relocation (std::vector<std::byte> payload,
                    std::chrono::hours retention,
                    std::chrono::steady_clock::time_point operation_deadline,
                    std::stop_token cancellation = {}) override
    {
        const auto check_operation = [&] () -> std::optional<task_t<relocation_stored_t>> {
            if (cancellation.stop_requested ())
                return cancelled<relocation_stored_t> ();
            if (std::chrono::steady_clock::now () >= operation_deadline)
                return failed<relocation_stored_t> (framework_error_kind_t::deadline_exceeded,
                                                    "relocation Store operation deadline elapsed");
            return std::nullopt;
        };
        if (auto terminal = check_operation ())
            return std::move (*terminal);
        if (retention <= std::chrono::hours::zero ())
            return failed<relocation_stored_t> (framework_error_kind_t::protocol_error,
                                                "relocation retention must be positive");

        const auto retention_ms = std::chrono::duration_cast<std::chrono::milliseconds> (retention);
        const auto checksum =
          zlink::framework::detail::crc32c (std::span<const std::byte> (payload));
        auto reference = make_reference ();
        for (;;) {
            if (auto terminal = check_operation ())
                return std::move (*terminal);
            auto pending_put = _store->put (
              blob_reference_t{reference},
              std::span<const std::byte> (payload.data (), payload.size ()), retention_ms);
            auto response = detail::observe_task_result_for (
              pending_put,
              std::chrono::ceil<std::chrono::milliseconds> (operation_deadline
                                                            - std::chrono::steady_clock::now ()),
              cancellation);
            if (auto terminal = check_operation ()) {
                detail::observe_task_terminal (pending_put, [payload = std::move (payload)] (
                                                              const result_t<blob_put_result_t> &) {
                    static_cast<void> (payload);
                });
                return std::move (*terminal);
            }
            auto result = std::move (*response);
            if (result) {
                const auto &written = result.value ();
                if (const auto *stored = std::get_if<blob_stored_t> (&written))
                    return completed (relocation_stored_t{reference, checksum, stored->expires_at,
                                                          stored->store_now});
                if (const auto *stored = std::get_if<blob_already_stored_t> (&written))
                    return completed (relocation_stored_t{reference, checksum, stored->expires_at,
                                                          stored->store_now});
                reference = make_reference ();
                continue;
            }

            if (auto terminal = check_operation ())
                return std::move (*terminal);
            auto pending_read = _store->read (blob_reference_t{reference});
            auto read_response = detail::observe_task_result_for (
              pending_read,
              std::chrono::ceil<std::chrono::milliseconds> (operation_deadline
                                                            - std::chrono::steady_clock::now ()),
              cancellation);
            if (auto terminal = check_operation ())
                return std::move (*terminal);
            auto read = std::move (*read_response);
            if (!read)
                return task_t<relocation_stored_t> (detail::propagate_failure<relocation_stored_t> (
                  read, "relocation Store read-back failed"));
            if (const auto *found = std::get_if<blob_found_t> (&read.value ())) {
                if (found->bytes != payload) {
                    reference = make_reference ();
                    continue;
                }
                return completed (
                  relocation_stored_t{reference, checksum, found->expires_at, found->store_now});
            }
        }
    }

    task_t<relocation_read_result_t> get_relocation (std::string reference,
                                                     std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            co_return co_await cancelled<relocation_read_result_t> ();
        auto pending = _store->read (blob_reference_t{std::move (reference)});
        auto result = co_await await_result (std::move (pending));
        if (!result)
            co_return detail::propagate_failure<relocation_read_result_t> (
              result, "relocation Store read failed");
        if (const auto *found = std::get_if<blob_found_t> (&result.value ()))
            co_return relocation_read_result_t{relocation_found_t{found->bytes}};
        co_return relocation_read_result_t{relocation_missing_t{}};
    }

    task_t<relocation_renew_result_t> renew_relocation (std::string reference,
                                                        std::chrono::hours retention,
                                                        std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            co_return co_await cancelled<relocation_renew_result_t> ();
        if (retention <= std::chrono::hours::zero ())
            co_return co_await failed<relocation_renew_result_t> (
              framework_error_kind_t::protocol_error, "relocation retention must be positive");
        auto pending =
          _store->renew (blob_reference_t{std::move (reference)},
                         std::chrono::duration_cast<std::chrono::milliseconds> (retention));
        auto result = co_await await_result (std::move (pending));
        if (!result)
            co_return detail::propagate_failure<relocation_renew_result_t> (
              result, "relocation Store renew failed");
        if (const auto *renewed = std::get_if<blob_renewed_t> (&result.value ()))
            co_return relocation_renew_result_t{
              relocation_renewed_t{renewed->expires_at, renewed->store_now}};
        co_return relocation_renew_result_t{relocation_renew_missing_t{}};
    }

    task_t<relocation_delete_result_t>
    delete_relocation (std::string reference, std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            co_return co_await cancelled<relocation_delete_result_t> ();
        auto pending = _store->erase (blob_reference_t{std::move (reference)});
        auto result = co_await await_result (std::move (pending));
        if (!result)
            co_return detail::propagate_failure<relocation_delete_result_t> (
              result, "relocation Store erase failed");
        co_return relocation_delete_result_t::deleted;
    }

  private:
    static constexpr std::string_view reference_prefix = "zlr-";

    template <typename T> static task_t<T> completed (T value)
    {
        return task_t<T> (result_t<T>::success (std::move (value)));
    }

    template <typename T> static task_t<T> failed (framework_error_kind_t kind, std::string message)
    {
        return task_t<T> (result_t<T>::failure (kind, std::move (message)));
    }

    template <typename T> static task_t<T> cancelled ()
    {
        return task_t<T> (detail::result_access_t::failure<T> (
          detail::make_cancellation_exception ("relocation Store operation was cancelled")));
    }

    static std::string make_reference ()
    {
        std::array<std::uint64_t, 2> words{};
        std::random_device random;
        for (auto &word : words)
            word = (static_cast<std::uint64_t> (random ()) << 32)
                   | static_cast<std::uint64_t> (random ());
        std::ostringstream stream;
        stream << reference_prefix << std::hex << std::setfill ('0');
        for (const auto word : words)
            stream << std::setw (16) << word;
        return stream.str ();
    }

    relocation_store_t *_store;
};

} // namespace zlink::framework::runtime

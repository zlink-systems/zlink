/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/protocol/service_wire_codec.hpp"

#include <chrono>
#include <array>
#include <cerrno>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <stdexcept>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#elif defined(__linux__)
#include <sys/random.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) \
  || defined(__OpenBSD__) || defined(__NetBSD__)
#include <cstdlib>
#else
#error "Relocation ID generation requires an operating-system CSPRNG"
#endif

namespace zlink::framework::runtime
{

class relocation_id_generator_t final
{
  public:
    using candidate_source_t =
      std::function<protocol::relocation_id_t ()>;

    relocation_id_generator_t () :
        relocation_id_generator_t (secure_candidate)
    {
    }

    explicit relocation_id_generator_t (candidate_source_t source) :
        _source (std::move (source))
    {
        if (!_source)
            throw std::invalid_argument (
              "Relocation ID candidate source is required");
    }

    protocol::relocation_id_t issue ()
    {
        std::lock_guard lock (_mutex);
        const auto now = std::chrono::steady_clock::now ();
        while (!_retained.empty () && _retained.front ().first <= now) {
            _issued.erase (key (_retained.front ().second));
            _retained.pop_front ();
        }
        for (std::size_t attempt = 0; attempt != 64; ++attempt) {
            const auto candidate = _source ();
            if ((candidate.high == 0 && candidate.low == 0)
                || !_issued.emplace (key (candidate)).second) {
                continue;
            }
            _retained.emplace_back (now + retention, candidate);
            return candidate;
        }
        throw std::runtime_error (
          "Relocation ID generation exhausted collision retries");
    }

  private:
    using id_key_t = std::pair<std::uint64_t, std::uint64_t>;

    static constexpr auto retention = std::chrono::hours (24);

    static id_key_t key (const protocol::relocation_id_t &value) noexcept
    {
        return {value.high, value.low};
    }

    static protocol::relocation_id_t secure_candidate ()
    {
        std::array<unsigned char, 16> bytes{};
#if defined(_WIN32)
        if (BCryptGenRandom (
              nullptr, bytes.data (), static_cast<ULONG> (bytes.size ()),
              BCRYPT_USE_SYSTEM_PREFERRED_RNG)
            != 0) {
            throw std::runtime_error (
              "Platform CSPRNG is unavailable for Relocation ID generation");
        }
#elif defined(__linux__)
        std::size_t offset = 0;
        while (offset != bytes.size ()) {
            const auto read = getrandom (
              bytes.data () + offset, bytes.size () - offset, 0);
            if (read < 0) {
                if (errno == EINTR)
                    continue;
                throw std::runtime_error (
                  "Platform CSPRNG is unavailable for Relocation ID generation");
            }
            if (read == 0)
                throw std::runtime_error (
                  "Platform CSPRNG returned an incomplete Relocation ID");
            offset += static_cast<std::size_t> (read);
        }
#else
        arc4random_buf (bytes.data (), bytes.size ());
#endif
        protocol::relocation_id_t result;
        for (std::size_t index = 0; index != 8; ++index)
            result.high = (result.high << 8u) | bytes[index];
        for (std::size_t index = 8; index != bytes.size (); ++index)
            result.low = (result.low << 8u) | bytes[index];
        return result;
    }

    candidate_source_t _source;
    std::mutex _mutex;
    std::set<id_key_t> _issued;
    std::deque<std::pair<
      std::chrono::steady_clock::time_point,
      protocol::relocation_id_t>> _retained;
};

} // namespace zlink::framework::runtime

/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PERF_DEALER_DEALER_RECEIVE_HPP
#define PERF_DEALER_DEALER_RECEIVE_HPP

#include "perf_metric_header.hpp"

namespace perf::multi
{
// Return the phase match used by latency sampling and Auto-HWM reporting.
inline bool record_dealer_dealer_receive (
  const perf_metric::header_t &header,
  perf_metric::phase_t phase,
  size_t msg_size,
  std::chrono::steady_clock::time_point now,
  std::chrono::steady_clock::time_point sample_start,
  std::chrono::steady_clock::time_point deadline,
  uint64_t &count)
{
    const bool matched = perf_metric::is_expected (header, 1U, phase, msg_size);
    if (now >= sample_start && now < deadline
        && (matched
            || (phase == perf_metric::phase_latency
                && perf_metric::is_expected (header, 1U, perf_metric::phase_warmup, msg_size))))
        ++count;
    return matched;
}
} // namespace perf::multi

#endif

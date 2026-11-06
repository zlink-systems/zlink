/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

// Included only by instrumented test builds; no production declarations or storage.
#if defined(ZLINK_FRAMEWORK_TEST_CREATION_PROBE)
#include <atomic>
#include <cstddef>

namespace zlink::framework::detail::test_creation_probe
{
inline std::atomic_size_t shared_states{0};
inline std::atomic_size_t coroutine_states{0};
inline std::atomic_size_t payload_copies{0};
inline std::atomic_size_t request_deliveries{0};
inline std::atomic_size_t request_capture_bytes{0};
inline std::atomic_size_t request_capture_value_bytes{0};
inline std::atomic_size_t request_block_bytes{0};
inline std::atomic_size_t request_callback_owner_bytes{0};
} // namespace zlink::framework::detail::test_creation_probe
#endif

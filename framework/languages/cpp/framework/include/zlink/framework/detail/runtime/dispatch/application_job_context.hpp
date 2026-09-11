/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <utility>

namespace zlink::framework::detail
{

// Internal application job invocation context (Java ApplicationJobContext /
// .NET ApplicationJobQueueInvocation). Public task templates carry this
// opaque identity, so its storage must also work without a runtime library.
// The context owns no admission policy and never dereferences the identity.
class application_job_context_t
{
  public:
    static const void *current () noexcept { return _current; }

    static const void *exchange (const void *job) noexcept
    {
        return std::exchange (_current, job);
    }

  private:
    inline static thread_local constinit const void *_current = nullptr;
};

} // namespace zlink::framework::detail

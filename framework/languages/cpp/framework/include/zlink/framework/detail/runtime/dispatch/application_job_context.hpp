/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

namespace zlink::framework::detail
{

// Internal application job invocation context (Java ApplicationJobContext /
// .NET ApplicationJobQueueInvocation). Public task templates carry this
// opaque identity across the application and runtime library boundary.
// The context owns no admission policy and never dereferences the identity.
class application_job_context_t
{
  public:
    static const void *current () noexcept;
    static const void *exchange (const void *job) noexcept;
};

} // namespace zlink::framework::detail

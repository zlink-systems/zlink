/* SPDX-License-Identifier: Elastic-2.0 */
#include "runtime/spots/spot_runtime.hpp"

#include <gtest/gtest.h>

namespace zlink::framework
{
namespace
{
TEST (spot_admission_rejection, reports_the_close_reason_without_changing_the_kind)
{
    detail::spot_context_state_t state;
    state.admission_sealed = true;

    for (const auto reason :
         {spot_close_reason_t::explicit_close, spot_close_reason_t::idle_evicted,
          spot_close_reason_t::host_shutdown, spot_close_reason_t::relocation_out}) {
        SCOPED_TRACE (static_cast<int> (reason));
        state.closed = reason;
        ASSERT_FALSE (state.admit (true));
        const auto error = state.admission_error ();
        switch (reason) {
            case spot_close_reason_t::explicit_close:
            case spot_close_reason_t::idle_evicted:
                EXPECT_EQ (error.kind (), framework_error_kind_t::not_found);
                EXPECT_STREQ (error.what (), "SPOT is closing.");
                break;
            case spot_close_reason_t::host_shutdown:
                EXPECT_EQ (error.kind (), framework_error_kind_t::shutting_down);
                EXPECT_STREQ (error.what (), "SPOT is closed.");
                break;
            case spot_close_reason_t::relocation_out:
                EXPECT_EQ (error.kind (), framework_error_kind_t::rejected);
                EXPECT_STREQ (error.what (), "Spot admission is sealed");
                break;
        }
    }
}
}
}

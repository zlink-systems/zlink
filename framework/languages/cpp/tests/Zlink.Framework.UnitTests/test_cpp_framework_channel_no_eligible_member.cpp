/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework.hpp>

#include "runtime/messaging/submit_result_mapper.hpp"

#include <gtest/gtest.h>

#include <string>

/*
 * 06-framework-api "no eligible select-one member": applying eligibility and
 * drain can leave a select-one channel with no member to pick while the send
 * path and its connection are still there. That ends as unavailable, not
 * not_found, and a request and a one-way send agree. A node-direct target that
 * is genuinely absent still ends as not_found.
 */
namespace
{

using zlink::framework::framework_error_kind_t;
using zlink::framework::runtime::messaging::map_channel_submit_result_exception;
using zlink::framework::runtime::messaging::map_submit_result_exception;

TEST (CppFrameworkChannelNoEligibleMember, ChannelNotFoundEndsUnavailable)
{
    const auto mapped = map_channel_submit_result_exception (
      zlink::submit_result_t::not_found, "channel had no eligible member");
    EXPECT_EQ (framework_error_kind_t::unavailable, mapped.kind ());
}

TEST (CppFrameworkChannelNoEligibleMember, NodeDirectNotFoundStaysNotFound)
{
    const auto mapped = map_submit_result_exception (
      zlink::submit_result_t::not_found, "node direct target is absent");
    EXPECT_EQ (framework_error_kind_t::not_found, mapped.kind ());
}

TEST (CppFrameworkChannelNoEligibleMember, ChannelKeepsEveryOtherSubmitResult)
{
    EXPECT_EQ (framework_error_kind_t::unavailable,
               map_channel_submit_result_exception (
                 zlink::submit_result_t::not_connected, "not connected")
                 .kind ());
    EXPECT_EQ (framework_error_kind_t::shutting_down,
               map_channel_submit_result_exception (
                 zlink::submit_result_t::terminated, "terminated")
                 .kind ());
    EXPECT_EQ (framework_error_kind_t::deadline_exceeded,
               map_channel_submit_result_exception (
                 zlink::submit_result_t::backpressured, "backpressured")
                 .kind ());
    EXPECT_EQ (framework_error_kind_t::rejected,
               map_channel_submit_result_exception (
                 zlink::submit_result_t::not_admitted, "not admitted")
                 .kind ());
}

} // namespace

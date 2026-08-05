/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace
{

class hosted_service_mock_t final : public zlink::framework::hosted_service_t
{
  public:
    MOCK_METHOD (void, start, (zlink::framework::service_provider_t & services), (override));
    MOCK_METHOD (void, stop, (), (noexcept, override));
};

} // namespace

TEST (FrameworkGoogleTestHarness, SupportsGoogleMockBoundaries)
{
    hosted_service_mock_t service;
    EXPECT_CALL (service, stop ()).Times (1);
    service.stop ();
}

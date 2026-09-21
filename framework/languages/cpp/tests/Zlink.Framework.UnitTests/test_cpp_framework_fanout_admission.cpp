/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework.hpp>

#include "runtime/channels/channel_runtime.hpp"

#include <zlink/Contracts/Errors/errors.hpp>

#include <gtest/gtest.h>

#include <cerrno>
#include <future>
#include <string>

namespace
{

struct fanout_record_t
{
    static constexpr const char *packet_name = "fanout.admission.record";
    std::string payload;
};

struct fanout_test_runtime_t
{
    fanout_test_runtime_t ()
    {
        serializers.add<fanout_record_t> (
          [] (const fanout_record_t &value) {
              return zlink::framework::encoded_payload_t::from_string (value.payload);
          },
          [] (const zlink::framework::encoded_payload_t &payload) {
              return fanout_record_t{payload.to_string ()};
          },
          "application/x-zlink-fanout-admission");
        builder.channel ("events").enable_publisher (false).connect (
          "inproc://fanout-admission-test");
        runtime.bind_serializers (serializers);
    }

    zlink::framework::result_t<void> publish ()
    {
        return builder.publisher ()
          .publish ("events", "fanout.admission", fanout_record_t{"payload"})
          .async ()
          .result ();
    }

    zlink::framework::serializer_registry_t serializers;
    zlink::framework::zlink_builder_t builder;
    zlink::framework::detail::channel_runtime_t runtime =
      zlink::framework::detail::channel_runtime_t::from (builder.message_bus ());
};

TEST (cpp_framework_fanout_admission,
      publish_succeeds_when_queue_capacity_opens_before_send_timeout)
{
    fanout_test_runtime_t test;
    std::promise<void> entered;
    std::promise<void> capacity;
    auto capacity_ready = capacity.get_future ().share ();
    test.runtime.bind_fanout_transport (
      "events",
      [&] (std::string, std::string, std::string, zlink::message_t,
           std::chrono::milliseconds) -> zlink::framework::task_t<void> {
          entered.set_value ();
          capacity_ready.wait ();
          co_return;
      });

    auto pending = std::async (std::launch::async, [&] { return test.publish (); });
    entered.get_future ().wait ();
    EXPECT_EQ (pending.wait_for (std::chrono::milliseconds (0)), std::future_status::timeout);
    capacity.set_value ();

    ASSERT_EQ (pending.wait_for (std::chrono::seconds (5)), std::future_status::ready);
    EXPECT_TRUE (pending.get ());
}

TEST (cpp_framework_fanout_admission, publish_queue_send_timeout_maps_to_deadline_exceeded)
{
    fanout_test_runtime_t test;
    test.runtime.bind_fanout_transport (
      "events",
      [] (std::string, std::string, std::string, zlink::message_t,
          std::chrono::milliseconds) -> zlink::framework::task_t<void> {
          throw zlink::submit_error_t (zlink::submit_result_t::backpressured, ETIMEDOUT);
      });

    const auto result = test.publish ();

    ASSERT_FALSE (result);
    EXPECT_EQ (result.error_kind (), zlink::framework::framework_error_kind_t::deadline_exceeded);
}

} // namespace

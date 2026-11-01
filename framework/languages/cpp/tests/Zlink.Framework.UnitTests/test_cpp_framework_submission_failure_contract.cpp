/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "../support/runtime_failure_fixture.hpp"

#include <zlink/framework.hpp>
#include "runtime/channels/channel_runtime.hpp"

#include <zlink/Contracts/Errors/errors.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <typeinfo>

namespace
{
namespace fw = zlink::framework;

class submission_failure_t final : public std::runtime_error
{
  public:
    explicit submission_failure_t (std::shared_ptr<const int> marker) :
        std::runtime_error ("submission failure cause"), marker (std::move (marker))
    {
    }
    std::shared_ptr<const int> marker;
};

struct nonstandard_submission_failure_t
{
    std::shared_ptr<const int> marker;
    std::string detail;
};

fw::request_task_t<zlink::message_t> delayed_submission (fw::task_t<void> release,
                                                         std::exception_ptr failure)
{
    co_await release;
    std::rethrow_exception (failure);
    co_return zlink::message_t{};
}

class CppFrameworkSubmissionFailure : public ::testing::TestWithParam<bool>
{
  protected:
    fw::result_t<std::string> submit (std::exception_ptr failure)
    {
        auto builder = fw::test::runtime_failure_builder ();
        builder.channel ("submission-client").enable_client ().connect ("tcp://127.0.0.1:1");
        fw::serializer_registry_t serializers;
        auto runtime = fw::detail::channel_runtime_t::from (builder.message_bus ());
        runtime.bind_serializers (serializers);
        fw::task_completion_source_t<void> release;
        int attempts = 0;
        runtime.bind_client_server_transport (
          "submission-client", {},
          [&] (std::string, std::string, zlink::message_t, std::chrono::milliseconds,
               std::map<std::string, std::string>) -> fw::request_task_t<zlink::message_t> {
              ++attempts;
              if (!GetParam ())
                  std::rethrow_exception (failure);
              return delayed_submission (release.task (), failure);
          });
        auto pending = builder.message_bus ()
                         .request ("submission-client", std::string ("request"))
                         .async<std::string> ();
        release.complete (fw::result_t<void>::success ());
        auto result = pending.result ();
        EXPECT_EQ (attempts, 1);
        return result;
    }
};

TEST_P (CppFrameworkSubmissionFailure, UnexpectedFailureRetainsDynamicCause)
{
    const auto marker = std::make_shared<const int> (2360);
    const auto result = submit (std::make_exception_ptr (submission_failure_t (marker)));
    ASSERT_FALSE (result);
    ASSERT_NE (result.error (), nullptr);
    EXPECT_EQ (result.error_kind (), fw::framework_error_kind_t::internal_failure);
    try {
        result.value ();
        FAIL () << "request must fail";
    }
    catch (const fw::framework_exception_t &error) {
        const auto *nested = dynamic_cast<const std::nested_exception *> (&error);
        ASSERT_NE (nested, nullptr) << "the original exception must remain available";
        ASSERT_TRUE (nested->nested_ptr ());
        try {
            nested->rethrow_nested ();
        }
        catch (const submission_failure_t &cause) {
            EXPECT_EQ (typeid (cause), typeid (submission_failure_t));
            EXPECT_EQ (cause.marker, marker);
            EXPECT_STREQ (cause.what (), "submission failure cause");
        }
        catch (...) {
            FAIL () << "the original exception dynamic type must be preserved";
        }
    }
}

TEST_P (CppFrameworkSubmissionFailure, FrameworkKindsRemainUnchanged)
{
    for (int kind = 0; kind <= static_cast<int> (fw::framework_error_kind_t::internal_failure);
         ++kind) {
        const auto expected = static_cast<fw::framework_error_kind_t> (kind);
        const auto result =
          submit (std::make_exception_ptr (fw::framework_exception_t (expected, "typed failure")));
        ASSERT_FALSE (result);
        EXPECT_EQ (result.error_kind (), expected);
        EXPECT_STREQ (result.error ()->what (), "typed failure");
    }
}

TEST_P (CppFrameworkSubmissionFailure, CancellationRemainsStandardException)
{
    const auto result = submit (std::make_exception_ptr (
      std::system_error (std::make_error_code (std::errc::operation_canceled), "cancelled")));
    ASSERT_FALSE (result);
    EXPECT_EQ (result.error (), nullptr);
    try {
        result.value ();
        FAIL () << "request must be cancelled";
    }
    catch (const std::system_error &error) {
        EXPECT_EQ (error.code (), std::errc::operation_canceled);
    }
}

TEST_P (CppFrameworkSubmissionFailure, BindingRequestResultRemainsNotFound)
{
    const auto result = submit (
      std::make_exception_ptr (zlink::request_error_t (zlink::request_result_t::not_found, 0)));
    ASSERT_FALSE (result);
    EXPECT_EQ (result.error_kind (), fw::framework_error_kind_t::not_found);
}

INSTANTIATE_TEST_SUITE_P (PublicChannelRequest, CppFrameworkSubmissionFailure, ::testing::Bool ());

TEST (CppFrameworkSubmissionLifetime, AdmissionFailureRetainsPhysicalRequest)
{
    auto builder = fw::test::runtime_failure_builder ();
    builder.channel ("submission-lifetime").enable_client ().connect ("tcp://127.0.0.1:1");
    fw::serializer_registry_t serializers;
    auto runtime = fw::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    auto admission = std::make_shared<fw::task_completion_source_t<void>> ();
    fw::task_completion_source_t<zlink::message_t> physical;
    runtime.bind_client_server_transport (
      "submission-lifetime", {},
      [&] (std::string, std::string, zlink::message_t, std::chrono::milliseconds,
           std::map<std::string, std::string>) {
          auto reply = fw::detail::with_task_resume_scheduler (
            physical.task (), [] (std::function<void ()> work) { work (); });
          return fw::request_task_t<zlink::message_t> (std::move (reply), admission);
      });
    auto pending = builder.message_bus ()
                     .request ("submission-lifetime", std::string ("request"))
                     .async<std::string> ();
    EXPECT_EQ (runtime.pending_count (), 1);
    const auto marker = std::make_shared<const int> (2360);
    const auto original = std::make_exception_ptr (submission_failure_t (marker));
    admission->complete (fw::detail::result_access_t::failure<void> (original));
    auto admitted_stage = pending.admitted ();
    const bool admitted_ready = admitted_stage.await_ready ();
    std::optional<fw::result_t<std::string>> first_result;
    if (pending.await_ready ())
        first_result.emplace (pending.result ());
    const auto retained_count = runtime.pending_count ();
    physical.complete (fw::detail::result_access_t::failure<zlink::message_t> (original));
    const auto result = pending.result ();
    const auto admitted = admitted_stage.result ();
    EXPECT_TRUE (admitted_ready);
    EXPECT_TRUE (first_result.has_value ());
    EXPECT_EQ (admitted.exception (), result.exception ());
    EXPECT_EQ (retained_count, 1);
    EXPECT_EQ (runtime.pending_count (), 0);
    if (first_result)
        EXPECT_EQ (first_result->exception (), result.exception ());
    ASSERT_FALSE (result);
    ASSERT_NE (result.error (), nullptr);
    EXPECT_EQ (result.error_kind (), fw::framework_error_kind_t::internal_failure);
    try {
        result.value ();
        FAIL () << "admission must fail";
    }
    catch (const fw::framework_exception_t &error) {
        const auto *nested = dynamic_cast<const std::nested_exception *> (&error);
        ASSERT_NE (nested, nullptr);
        ASSERT_TRUE (nested->nested_ptr ());
        try {
            nested->rethrow_nested ();
        }
        catch (const submission_failure_t &cause) {
            EXPECT_EQ (typeid (cause), typeid (submission_failure_t));
            EXPECT_EQ (cause.marker, marker);
            EXPECT_STREQ (cause.what (), "submission failure cause");
        }
        catch (...) {
            FAIL () << "the admission cause dynamic type must be preserved";
        }
    }
}

TEST (CppFrameworkSubmissionLifetime, NonStandardAdmissionFailureRetainsDynamicCause)
{
    auto builder = fw::test::runtime_failure_builder ();
    builder.channel ("nonstandard-submission").enable_client ().connect ("tcp://127.0.0.1:1");
    fw::serializer_registry_t serializers;
    auto runtime = fw::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    auto admission = std::make_shared<fw::task_completion_source_t<void>> ();
    fw::task_completion_source_t<zlink::message_t> physical;
    int attempts = 0;
    runtime.bind_client_server_transport (
      "nonstandard-submission", {},
      [&] (std::string, std::string, zlink::message_t, std::chrono::milliseconds,
           std::map<std::string, std::string>) {
          ++attempts;
          auto reply = fw::detail::with_task_resume_scheduler (
            physical.task (), [] (std::function<void ()> work) { work (); });
          return fw::request_task_t<zlink::message_t> (std::move (reply), admission);
      });
    auto pending = builder.message_bus ()
                     .request ("nonstandard-submission", std::string ("request"))
                     .async<std::string> ();
    const auto marker = std::make_shared<const int> (2360);
    const auto original = std::make_exception_ptr (
      nonstandard_submission_failure_t{marker, "non-standard admission failure"});
    admission->complete (fw::detail::result_access_t::failure<void> (original));
    auto admitted_stage = pending.admitted ();
    const bool admitted_ready = admitted_stage.await_ready ();
    std::optional<fw::result_t<std::string>> first_result;
    if (pending.await_ready ())
        first_result.emplace (pending.result ());
    const auto retained_count = runtime.pending_count ();
    physical.complete (fw::detail::result_access_t::failure<zlink::message_t> (original));
    const auto result = pending.result ();
    const auto admitted = admitted_stage.result ();
    EXPECT_EQ (attempts, 1);
    EXPECT_TRUE (admitted_ready);
    EXPECT_TRUE (first_result.has_value ());
    EXPECT_EQ (admitted.exception (), result.exception ());
    EXPECT_EQ (retained_count, 1);
    EXPECT_EQ (runtime.pending_count (), 0);
    if (first_result)
        EXPECT_EQ (first_result->exception (), result.exception ());
    ASSERT_FALSE (result);
    ASSERT_NE (result.error (), nullptr);
    EXPECT_EQ (result.error_kind (), fw::framework_error_kind_t::internal_failure);
    EXPECT_EQ (admitted.error_kind (), fw::framework_error_kind_t::internal_failure);
    try {
        result.value ();
        FAIL () << "non-standard admission failure must fail the request";
    }
    catch (const fw::framework_exception_t &error) {
        const auto *nested = dynamic_cast<const std::nested_exception *> (&error);
        ASSERT_NE (nested, nullptr);
        ASSERT_TRUE (nested->nested_ptr ());
        try {
            nested->rethrow_nested ();
        }
        catch (const nonstandard_submission_failure_t &cause) {
            EXPECT_EQ (cause.marker, marker);
            EXPECT_EQ (cause.detail, "non-standard admission failure");
        }
        catch (...) {
            FAIL () << "the original non-standard exception must be preserved";
        }
    }
}

TEST (CppFrameworkSubmissionValidation, InvalidArgumentRemainsSynchronous)
{
    auto app = fw::app_t::create ();
    EXPECT_THROW (app.add_zlink_framework ([] (fw::zlink_framework_options_t &options) {
        options.add_fanout_channel ("invalid-timeout")
          .set_send_timeout (std::chrono::milliseconds::zero ());
    }),
                  std::invalid_argument);
}

TEST (CppFrameworkSubmissionValidation, MissingConfigurationRemainsSynchronous)
{
    const fw::configuration_model_t configuration;
    const fw::configuration_section_t section (configuration, "submission");
    try {
        section.require ("required-value");
        FAIL () << "missing configuration must fail before submission";
    }
    catch (const fw::framework_exception_t &error) {
        EXPECT_EQ (error.kind (), fw::framework_error_kind_t::protocol_error);
        EXPECT_STREQ (error.what (),
                      "required configuration value missing: submission.required-value");
    }
}
// Decode failures must retain the exact local callback exception through both
// the handler result and the typed request reply projection.
struct decode_payload_t
{
};

struct decode_handler_t
{
    std::string request (const decode_payload_t &)
    {
        ADD_FAILURE () << "decode failure must precede the request handler";
        return {};
    }
    void send (const decode_payload_t &)
    {
        ADD_FAILURE () << "decode failure must precede the send handler";
    }
};

class CppFrameworkDecodeFailure : public ::testing::TestWithParam<int>
{
  protected:
    void SetUp () override
    {
        if (GetParam () == 0)
            original = std::make_exception_ptr (std::runtime_error ("decoder cause"));
        else if (GetParam () == 1)
            original = std::make_exception_ptr (nonstandard_submission_failure_t{
              std::make_shared<const int> (3472), "non-standard decoder cause"});
        else {
            try {
                throw std::runtime_error ("nested decoder cause");
            }
            catch (...) {
                try {
                    std::throw_with_nested (fw::framework_exception_t (
                      fw::framework_error_kind_t::protocol_error, "typed decoder cause"));
                }
                catch (...) {
                    original = std::current_exception ();
                }
            }
        }
        serializers.add<decode_payload_t> (
          [] (const decode_payload_t &) { return fw::encoded_payload_t::from_string ("payload"); },
          [this] (const fw::encoded_payload_t &) -> decode_payload_t {
              std::rethrow_exception (original);
          },
          "application/x-decode-cause");
    }

    template <typename T> void expect_cause (const fw::result_t<T> &result)
    {
        ASSERT_FALSE (result);
        ASSERT_NE (result.error (), nullptr);
        EXPECT_EQ (result.error_kind (), fw::framework_error_kind_t::protocol_error);
        if (GetParam () == 2) {
            EXPECT_EQ (result.exception (), original);
            return;
        }
        try {
            result.value ();
            FAIL () << "decode must fail";
        }
        catch (const fw::framework_exception_t &error) {
            EXPECT_EQ (fw::detail::failure_origin (error),
                       fw::detail::failure_origin_t::payload_decode);
            const auto *nested = dynamic_cast<const std::nested_exception *> (&error);
            ASSERT_NE (nested, nullptr);
            EXPECT_EQ (nested->nested_ptr (), original);
        }
    }

    fw::serializer_registry_t serializers;
    std::exception_ptr original;
};

TEST_P (CppFrameworkDecodeFailure, RequestRetainsOriginalCause)
{
    fw::service_collection_t services;
    services.add_singleton<decode_handler_t> ();
    auto provider = services.build_provider ();
    fw::handler_registry_t handlers;
    handlers.on_request<decode_handler_t, decode_payload_t, std::string> (
      "decode", "request", &decode_handler_t::request, {.packet_name = "payload"});
    expect_cause (handlers.invoke ("decode", "request", "payload", provider, serializers,
                                   zlink::message_t::from (std::string ("payload"))));
}

TEST_P (CppFrameworkDecodeFailure, SendRetainsOriginalCause)
{
    fw::service_collection_t services;
    services.add_singleton<decode_handler_t> ();
    auto provider = services.build_provider ();
    fw::handler_registry_t handlers;
    handlers.on_send<decode_handler_t, decode_payload_t> ("decode", "send", &decode_handler_t::send,
                                                          {.packet_name = "payload"});
    // Exercise downstream result retention in addition to terminal dispatch.
    handlers.add_filter (
      [] (auto &, auto &, const auto &, fw::handler_next_t next) -> fw::task_t<void> {
          const auto completed = next ();
          EXPECT_TRUE (completed.await_ready ());
          EXPECT_FALSE (completed.result ());
          co_return;
      });
    expect_cause (handlers.invoke ("decode", "send", "payload", provider, serializers,
                                   zlink::message_t::from (std::string ("payload"))));
}

TEST_P (CppFrameworkDecodeFailure, ReplyRetainsOriginalCause)
{
    fw::channel_request_call_t call ("payload", &serializers,
                                     [] (const std::string &, std::chrono::milliseconds,
                                         const std::map<std::string, std::string> &) {
                                         return fw::request_task_t<zlink::message_t> (
                                           fw::result_t<zlink::message_t>::success (
                                             zlink::message_t::from (std::string ("payload"))));
                                     });
    expect_cause (call.async<decode_payload_t> ().result ());
}

TEST_P (CppFrameworkDecodeFailure, ReceiverReplyRetainsOriginalCause)
{
    auto builder = fw::test::runtime_failure_builder ();
    builder.channel ("decode-reply").enable_client ().connect ("tcp://127.0.0.1:1");
    auto runtime = fw::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    runtime.bind_client_server_transport (
      "decode-reply", {},
      [] (std::string, std::string, zlink::message_t, std::chrono::milliseconds,
          std::map<std::string, std::string>) {
          return fw::request_task_t<zlink::message_t> (fw::result_t<zlink::message_t>::success (
            zlink::message_t::from (std::string ("payload"))));
      });
    expect_cause (builder.message_bus ()
                    .request ("decode-reply", std::string ("request"))
                    .async<decode_payload_t> ()
                    .result ());
}

INSTANTIATE_TEST_SUITE_P (LocalDecoderCauses,
                          CppFrameworkDecodeFailure,
                          ::testing::Values (0, 1, 2));

} // namespace

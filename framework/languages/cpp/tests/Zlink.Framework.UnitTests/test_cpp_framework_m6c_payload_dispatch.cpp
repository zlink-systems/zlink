/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/m6c_runtime_cases.hpp"
#include "../support/request_admission_fixture.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <zlink/framework/contracts/configuration/framework_options.hpp>
#include <zlink/framework/contracts/configuration/zlink_builder.hpp>
#include <zlink/json.hpp>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace
{

struct temporary_channel_reply_t
{
    int value = 0;
};
struct accepted_decode_payload_t
{
    int value = 0;
};

struct rejected_decode_payload_t
{
    int value = 0;
};

struct failed_decode_payload_t
{
    int value = 0;
};

int successful_payload_deserializations = 0;
int other_payload_deserializations = 0;
int failed_payload_deserializations = 0;

} // namespace

template <> struct zlink::json::dto_traits<temporary_channel_reply_t>
{
    static constexpr auto fields =
      std::tuple{zlink::json::field ("value", &temporary_channel_reply_t::value)};
};

template <> struct zlink::json::dto_traits<accepted_decode_payload_t>
{
    static constexpr auto fields =
      std::tuple{zlink::json::field ("value", &accepted_decode_payload_t::value)};
};

template <> struct zlink::json::dto_traits<rejected_decode_payload_t>
{
    static constexpr auto fields =
      std::tuple{zlink::json::field ("value", &rejected_decode_payload_t::value)};
};

template <> struct zlink::json::dto_traits<failed_decode_payload_t>
{
    static constexpr auto fields =
      std::tuple{zlink::json::field ("value", &failed_decode_payload_t::value)};
};

namespace m6c_test
{

void test_temporary_channel_request_yield_owns_call_state (test_context_t &test)
{
    namespace detail = zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;
    using zlink::framework::channel_request_call_t;
    using zlink::framework::encoded_payload_t;
    using zlink::framework::serializer_registry_t;
    using zlink::framework::task_t;

    using reply_t = temporary_channel_reply_t;

    serializer_registry_t serializers;
    serializers.add<reply_t> (
      [] (const reply_t &value) {
          return encoded_payload_t::from_string (std::to_string (value.value));
      },
      [] (const encoded_payload_t &payload) { return reply_t{std::stoi (payload.to_string ())}; });

    auto state = std::make_shared<detail::spot_context_state_t> ();
    state->node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("temporary-channel-call-node");
    state->serial_executor =
      std::make_shared<runtime::offload_executor_t> (2, "temporary-channel-call");
    state->serial_queue = std::make_shared<runtime::serial_execution_queue_t> (
      *state->serial_executor, runtime::serial_execution_queue_options_t{},
      runtime::serial_execution_queue_t::error_handler_t{},
      runtime::serial_lane_policy_t::spot_wide ());

    auto reply_source =
      std::make_shared<zlink::framework::task_completion_source_t<zlink::message_t>> ();
    auto result_source = std::make_shared<zlink::framework::task_completion_source_t<reply_t>> ();
    auto result_task = result_source->task ();
    const auto submitted = state->run_serial_sync ("temporary-channel-call-yield", [&] {
        auto pending =
          channel_request_call_t ("temporary.reply", &serializers,
                                  [reply_source] (const std::string &, std::chrono::milliseconds,
                                                  const channel_request_call_t::metadata_map_t &) {
                                      return zlink::framework::test::accepted_request_task (
                                        reply_source->task ());
                                  })
            .yield<reply_t> ();
        detail::observe_task_completion (
          pending, [result_source] (const zlink::framework::result_t<reply_t> &result) {
              result_source->complete (result);
          });
    });

    reply_source->complete (
      zlink::framework::result_t<zlink::message_t>::success (zlink::message_t::from ("451")));
    const auto decoded = result_task.result ();
    test.require (
      submitted && decoded && decoded.value ().value == 451,
      "temporary channel request yield must retain serializers and submit state after suspension");
}


class payload_decode_spot_t final : public zlink::framework::spot_t<zlink::framework::actor_t>
{
  public:
    explicit payload_decode_spot_t (zlink::framework::spot_context_t context) :
        _context (std::move (context))
    {
    }

    zlink::framework::spot_context_t &context () noexcept override { return _context; }

    const zlink::framework::spot_context_t &context () const noexcept override { return _context; }

    void configure () override {}

    zlink::framework::task_t<zlink::framework::spot_create_response_t>
    on_create (const zlink::framework::message_t &request) override
    {
        if (!failure_mode) {
            const auto first = request.decode<accepted_decode_payload_t> ();
            const auto copied_request = request;
            const auto second = copied_request.decode<accepted_decode_payload_t> ();
            bool other_rejected = false;
            try {
                (void) request.decode<rejected_decode_payload_t> ();
            }
            catch (const zlink::framework::framework_exception_t &error) {
                other_rejected =
                  error.kind () == zlink::framework::framework_error_kind_t::protocol_error;
            }
            success_observed = first.value == 73 && second.value == 73 && other_rejected;
        } else {
            std::string first_error;
            std::string repeated_error;
            try {
                (void) request.decode<failed_decode_payload_t> ();
            }
            catch (const zlink::framework::framework_exception_t &error) {
                first_error = error.what ();
            }
            try {
                (void) request.decode<rejected_decode_payload_t> ();
            }
            catch (const zlink::framework::framework_exception_t &error) {
                repeated_error = error.what ();
            }
            failure_observed = !first_error.empty () && repeated_error == first_error;
        }
        co_return zlink::framework::spot_create_response_t::accept ();
    }

    zlink::framework::task_t<void> on_initialize () override { co_return; }

    zlink::framework::task_t<zlink::framework::spot_actor_join_result_t>
    on_actor_join (std::string_view, const zlink::framework::message_t &) override
    {
        co_return zlink::framework::spot_actor_join_result_t::reject ();
    }

    zlink::framework::task_t<void> on_actor_joined (zlink::framework::actor_t &) override
    {
        co_return;
    }

    zlink::framework::task_t<void> on_leave_actor (zlink::framework::actor_t &) override
    {
        co_return;
    }

    static inline bool failure_mode = false;
    static inline bool success_observed = false;
    static inline bool failure_observed = false;

  private:
    zlink::framework::spot_context_t _context;
};

void test_accepted_message_payload_is_deserialized_once (test_context_t &test)
{
    payload_decode_spot_t::failure_mode = false;
    payload_decode_spot_t::success_observed = false;
    payload_decode_spot_t::failure_observed = false;
    successful_payload_deserializations = 0;
    other_payload_deserializations = 0;
    failed_payload_deserializations = 0;

    zlink::framework::zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    auto mesh = builder.add_route_mesh ("decode-cache-mesh");
    mesh.add_spot_factory<payload_decode_spot_t> (
      "decode-cache",
      [] (zlink::framework::spot_context_t context) {
          return std::make_shared<payload_decode_spot_t> (std::move (context));
      },
      [] (auto &factory) { factory.disable_relocation (); });
    zlink::framework::serializer_registry_t serializers;
    zlink::framework::codec_registration_context_t decode_codecs (serializers);
    decode_codecs.add_serializer<accepted_decode_payload_t> (
      [] (const auto &v) {
          return zlink::framework::encoded_payload_t::from_string (
            zlink::detail::json_profile::encode (v));
      },
      [] (const auto &p) {
          ++successful_payload_deserializations;
          const auto text = p.to_string ();
          return zlink::detail::json_profile::decode<accepted_decode_payload_t> (text.begin (),
                                                                                 text.end ());
      },
      zlink::detail::json_profile::content_type);
    decode_codecs.add_serializer<rejected_decode_payload_t> (
      [] (const auto &v) {
          return zlink::framework::encoded_payload_t::from_string (
            zlink::detail::json_profile::encode (v));
      },
      [] (const auto &p) {
          ++other_payload_deserializations;
          const auto text = p.to_string ();
          return zlink::detail::json_profile::decode<rejected_decode_payload_t> (text.begin (),
                                                                                 text.end ());
      },
      zlink::detail::json_profile::content_type);
    decode_codecs.add_serializer<failed_decode_payload_t> (
      [] (const auto &v) {
          return zlink::framework::encoded_payload_t::from_string (
            zlink::detail::json_profile::encode (v));
      },
      [] (const zlink::framework::encoded_payload_t &) -> failed_decode_payload_t {
          ++failed_payload_deserializations;
          throw std::runtime_error ("expected accepted-payload decode failure");
      },
      zlink::detail::json_profile::content_type);

    zlink::framework::detail::channel_runtime_t::from (builder.message_bus ())
      .bind_serializers (serializers);
    auto found_runtime =
      zlink::framework::detail::spot_node_runtime_t::from (builder, "decode-cache-mesh");
    test.require (found_runtime.has_value (),
                  "accepted-payload decode test must resolve its Spot runtime");
    if (!found_runtime)
        return;
    auto runtime = *found_runtime;

    const auto success =
      runtime.get_or_create_spot ("decode-cache", zlink::framework::spot_id_t ("decode-success"),
                                  zlink::message_t::from (R"({"value":73})"));
    test.require (success.state == zlink::framework::spot_create_state_t::created
                    && payload_decode_spot_t::success_observed
                    && successful_payload_deserializations == 1
                    && other_payload_deserializations == 0,
                  "one admitted payload must preserve its first successful decode across repeated "
                  "and differently typed reads");

    payload_decode_spot_t::failure_mode = true;
    const auto failure =
      runtime.get_or_create_spot ("decode-cache", zlink::framework::spot_id_t ("decode-failure"),
                                  zlink::message_t::from (R"({"value":91})"));
    test.require (failure.state == zlink::framework::spot_create_state_t::created
                    && payload_decode_spot_t::failure_observed
                    && failed_payload_deserializations == 1 && other_payload_deserializations == 0,
                  "one admitted payload must preserve its first decode failure without invoking "
                  "another serializer");

    runtime.request_stop ();
    runtime.cancel_pending_dispatch ();
    runtime.cancel_pending_work ();
    runtime.release_native_handles ();
}

} // namespace m6c_test

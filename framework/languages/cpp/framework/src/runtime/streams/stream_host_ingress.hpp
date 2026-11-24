/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "runtime/diagnostics/metric_provider.hpp"
#include "runtime/dispatch/application_job_queue_capacity.hpp"
#include "runtime/streams/stream_runtime.hpp"
#include <zlink/detail/stream_packet_name.hpp>

namespace zlink::framework::runtime
{

// One ingress owner serves all STREAM listeners on the host node.
class stream_host_ingress_t
{
  public:
    explicit stream_host_ingress_t (
      const std::vector<stream_snapshot_t> &streams,
      std::uint32_t effective_processors =
        effective_application_job_processors (detect_application_job_queue_processor_limits ())) :
        _limit (static_cast<std::uint32_t> (std::min<std::uint64_t> (
          static_cast<std::uint64_t> (effective_processors) * 8, max_option_value)))
    {
        std::optional<std::uint32_t> registered_limit;
        for (const auto &stream : streams) {
            const auto &options = stream.authentication;
            const auto configured_limit = options.max_concurrent.value_or (_limit);
            if (configured_limit < 1 || configured_limit > max_option_value
                || options.retry_delay.count () < 1
                || options.retry_delay.count () > max_option_value) {
                throw framework_exception_t (
                  framework_error_kind_t::protocol_error,
                  "STREAM authentication concurrency and retry delay must be in 1..2147483647");
            }
            if (options.packet_name
                && zlink::detail::stream_wire::validate_packet_name (*options.packet_name)
                     != zlink::detail::stream_wire::packet_name_error_t::none) {
                throw framework_exception_t (framework_error_kind_t::protocol_error,
                                             "Invalid STREAM authentication packet name");
            }
            const auto limit = static_cast<std::uint32_t> (configured_limit);
            if (registered_limit
                && (*registered_limit != limit || _packet_name != options.packet_name
                    || _retry_delay != options.retry_delay)) {
                throw framework_exception_t (
                  framework_error_kind_t::protocol_error,
                  "STREAM authentication admission settings must agree on every listener");
            }
            registered_limit = limit;
            _packet_name = options.packet_name;
            _retry_delay = options.retry_delay;
        }
        if (registered_limit)
            _limit = *registered_limit;
    }

    ~stream_host_ingress_t ()
    {
        if (_gauge)
            _gauge->RemoveCallback (&observe_in_flight, this);
        if (_rejections)
            _rejections->RemoveCallback (&observe_rejections, this);
    }

    stream_host_ingress_t (const stream_host_ingress_t &) = delete;
    stream_host_ingress_t &operator= (const stream_host_ingress_t &) = delete;

    std::uint32_t limit () const noexcept { return _limit; }
    std::uint32_t in_flight () const noexcept
    {
        return _in_flight->load (std::memory_order_relaxed);
    }

    void connect_metrics (metric_provider_t provider)
    {
        if (!_packet_name || !provider || _provider
            || dynamic_cast<opentelemetry::metrics::NoopMeterProvider *> (provider.get ()))
            return;
        auto meter = provider->GetMeter ("zlink.framework");
        if (!meter)
            return;
        _provider = std::move (provider);
        _gauge = meter->CreateDoubleObservableGauge (
          "zlink.host.authentication_admission.in_flight", "", "{request}");
        _rejections = meter->CreateInt64ObservableCounter (
          "zlink.host.authentication_admission.rejections", "", "{request}");
        if (_gauge)
            _gauge->AddCallback (&observe_in_flight, this);
        if (_rejections)
            _rejections->AddCallback (&observe_rejections, this);
    }

    void reset_metrics () noexcept { _rejection_count.store (0, std::memory_order_relaxed); }

    template <typename Reject>
    result_t<void> publish (const detail::stream_runtime_t &runtime,
                            packet_stream_session_t &session,
                            stream_t &stream,
                            const detail::stream_header_t &header,
                            const zlink::message_t &payload,
                            application_job_queue_t::permit_t permit,
                            detail::stream_runtime_t::async_dispatch_completion_t completion,
                            detail::stream_runtime_t::async_dispatch_cancel_t cancelled,
                            const Reject &reject)
    {
        std::shared_ptr<void> lease;
        if (_packet_name && header.kind () == detail::stream_message_kind_t::request
            && header.packet_name () == *_packet_name) {
            lease = try_enter ();
            if (!lease) {
                _rejection_count.fetch_add (1, std::memory_order_relaxed);
                permit.release_without_handler ();
                detail::stream_metadata_t metadata;
                metadata.with ("$zlink.admission.retryAfterMs",
                               std::to_string (_retry_delay.count ()));
                reject (result_t<void>::failure (framework_error_kind_t::unavailable,
                                                 "Authentication admission is full."),
                        std::move (metadata));
                return result_t<void>::success ();
            }
        }

        detail::stream_runtime_t::async_dispatch_started_t started;
        if (header.kind () == detail::stream_message_kind_t::send
            || header.kind () == detail::stream_message_kind_t::request) {
            permit.mark_queued ();
            auto queued = std::make_shared<application_job_queue_t::permit_t> (std::move (permit));
            started = [queued = std::move (queued)] () mutable {
                queued->release_for_handler_entry ();
                queued.reset ();
            };
        } else {
            permit.release_without_handler ();
        }
        runtime.set_session_codec (stream, header.codec ());
        return runtime.dispatch_packet_async (session, stream, header, payload,
                                              std::move (completion), std::move (started),
                                              std::move (cancelled), std::move (lease));
    }

  private:
    static constexpr auto max_option_value = std::numeric_limits<std::int32_t>::max ();

    std::shared_ptr<void> try_enter () const
    {
        auto current = in_flight ();
        while (current < _limit) {
            if (_in_flight->compare_exchange_weak (current, current + 1,
                                                   std::memory_order_relaxed)) {
                return {_in_flight.get (), [counter = _in_flight] (void *) {
                            counter->fetch_sub (1, std::memory_order_relaxed);
                        }};
            }
        }
        return {};
    }

    static void observe_in_flight (opentelemetry::metrics::ObserverResult result, void *context)
    {
        const auto *ingress = static_cast<const stream_host_ingress_t *> (context);
        auto observer = opentelemetry::nostd::get<
          opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObserverResultT<double>>> (
          result);
        observer->Observe (ingress->in_flight ());
    }

    static void observe_rejections (opentelemetry::metrics::ObserverResult result, void *context)
    {
        const auto *ingress = static_cast<const stream_host_ingress_t *> (context);
        auto observer = opentelemetry::nostd::get<
          opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObserverResultT<std::int64_t>>> (
          result);
        observer->Observe (ingress->_rejection_count.load (std::memory_order_relaxed));
    }

    std::optional<std::string> _packet_name;
    std::uint32_t _limit;
    std::chrono::milliseconds _retry_delay{500};
    std::shared_ptr<std::atomic_uint32_t> _in_flight = std::make_shared<std::atomic_uint32_t> (0);
    std::atomic_int64_t _rejection_count{0};
    metric_provider_t _provider;
    opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObservableInstrument> _gauge;
    opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObservableInstrument> _rejections;
};

} // namespace zlink::framework::runtime

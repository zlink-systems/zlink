/* SPDX-License-Identifier: MPL-2.0 */

/* Config 11 (ObservabilityOps) server host. One binary hosts a play role:
 * spot mesh + room spot + shared spot route channel + STREAM session gateway
 * (play-a only) + evidence HTTP endpoints. Evidence follows the common
 * config-11 §3 minimal JSON arrays: metrics / drainEvents / peerRows. */

#include "../Shared/observability_contracts.hpp"
#include "Shared/observability_host.hpp"

#include <zlink/framework.hpp>
#include <zlink/locations/redis.hpp>

#include <nlohmann/json.hpp>

#include <opentelemetry/metrics/provider.h>
#include <opentelemetry/nostd/variant.h>
#include <opentelemetry/metrics/noop.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace obs = zlink::framework::e2e::observability_ops;
namespace fw = zlink::framework;

namespace
{

struct server_options_t
{
    std::string node_rid;
    std::string redis_endpoint;
    std::string redis_key_prefix;
    std::string http_endpoint;
    std::string route_endpoint;
    std::string peer_route_endpoint;
    std::string spot_router_endpoint;
    std::string spot_pub_endpoint;
    std::string stream_endpoint;
    std::string log_dir;
    std::string trace_mode;
    bool metrics_enabled;
    bool room_timer_enabled;

    static server_options_t bind (const fw::configuration_section_t &section)
    {
        return {.node_rid = section.require ("nodeRid"),
                .redis_endpoint = section.require ("redis.endpoint"),
                .redis_key_prefix = section.require ("redis.keyPrefix"),
                .http_endpoint = section.require ("httpEndpoint"),
                .route_endpoint = section.require ("routeEndpoint"),
                .peer_route_endpoint = section.get ("peerRouteEndpoint").value_or (""),
                .spot_router_endpoint = section.get ("spotRouterEndpoint").value_or (""),
                .spot_pub_endpoint = section.get ("spotPubEndpoint").value_or (""),
                .stream_endpoint = section.get ("streamEndpoint").value_or (""),
                .log_dir = section.require ("logDir"),
                .trace_mode = section.get ("traceMode").value_or ("normal"),
                .metrics_enabled = section.get ("metrics").value_or ("on") != "off",
                .room_timer_enabled = section.get ("roomTimer").value_or ("off") == "on"};
    }
};

struct metric_descriptor_t
{
    std::string name;
    std::string unit;
    std::string kind;
    std::string temporality;
};

class evidence_metric_capture_t
{
  public:
    void record_sync (const metric_descriptor_t &descriptor,
                      double value,
                      const opentelemetry::common::KeyValueIterable &attributes) noexcept
    {
        record (_synchronous, descriptor, value, attributes);
    }

    void begin_observable_collection () noexcept
    {
        std::lock_guard lock (_mutex);
        _observables.clear ();
    }

    void record_observable (const metric_descriptor_t &descriptor,
                            double value,
                            const opentelemetry::common::KeyValueIterable &attributes) noexcept
    {
        record (_observables, descriptor, value, attributes);
    }

    nlohmann::json snapshot () const
    {
        std::lock_guard lock (_mutex);
        nlohmann::json metrics = nlohmann::json::array ();
        for (const auto &metric : _synchronous)
            metrics.push_back (metric);
        for (const auto &metric : _observables)
            metrics.push_back (metric);
        return metrics;
    }

  private:
    static nlohmann::json tags_of (const opentelemetry::common::KeyValueIterable &attributes)
    {
        nlohmann::json tags = nlohmann::json::object ();
        attributes.ForEachKeyValue (
          [&] (opentelemetry::nostd::string_view key, opentelemetry::common::AttributeValue value) {
              if (const auto *string =
                    opentelemetry::nostd::get_if<opentelemetry::nostd::string_view> (&value))
                  tags[std::string (key.data (), key.size ())] =
                    std::string (string->data (), string->size ());
              else if (const auto *string = opentelemetry::nostd::get_if<const char *> (&value))
                  tags[std::string (key.data (), key.size ())] = *string;
              return true;
          });
        return tags;
    }

    void record (std::vector<nlohmann::json> &destination,
                 const metric_descriptor_t &descriptor,
                 double value,
                 const opentelemetry::common::KeyValueIterable &attributes) noexcept
    {
        nlohmann::json metric{{"name", descriptor.name}, {"unit", descriptor.unit},
                              {"kind", descriptor.kind}, {"temporality", descriptor.temporality},
                              {"value", value},          {"tags", tags_of (attributes)}};
        std::lock_guard lock (_mutex);
        destination.push_back (std::move (metric));
    }

    mutable std::mutex _mutex;
    std::vector<nlohmann::json> _synchronous;
    std::vector<nlohmann::json> _observables;
};

class evidence_observable_t
{
  public:
    virtual ~evidence_observable_t () = default;
    virtual void collect () noexcept = 0;
};

template <typename T, template <typename> class base_t>
class evidence_add_t final : public base_t<T>
{
  public:
    evidence_add_t (std::shared_ptr<evidence_metric_capture_t> capture,
                    metric_descriptor_t descriptor) :
        base_t<T> (descriptor.name.c_str (), "", descriptor.unit.c_str ()),
        _capture (std::move (capture)),
        _descriptor (std::move (descriptor))
    {
    }

    void Add (T value) noexcept override { record (value, empty_attributes ()); }

    void Add (T value, const opentelemetry::context::Context &) noexcept override
    {
        record (value, empty_attributes ());
    }

    void Add (T value, const opentelemetry::common::KeyValueIterable &attributes) noexcept override
    {
        record (value, attributes);
    }

    void Add (T value,
              const opentelemetry::common::KeyValueIterable &attributes,
              const opentelemetry::context::Context &) noexcept override
    {
        record (value, attributes);
    }

  private:
    static const opentelemetry::common::NoopKeyValueIterable &empty_attributes ()
    {
        static const opentelemetry::common::NoopKeyValueIterable value;
        return value;
    }

    void record (T value, const opentelemetry::common::KeyValueIterable &attributes) noexcept
    {
        _capture->record_sync (_descriptor, static_cast<double> (value), attributes);
    }

    std::shared_ptr<evidence_metric_capture_t> _capture;
    metric_descriptor_t _descriptor;
};

template <typename T>
class evidence_histogram_t final : public opentelemetry::metrics::NoopHistogram<T>
{
  public:
    evidence_histogram_t (std::shared_ptr<evidence_metric_capture_t> capture,
                          metric_descriptor_t descriptor) :
        opentelemetry::metrics::NoopHistogram<T> (
          descriptor.name.c_str (), "", descriptor.unit.c_str ()),
        _capture (std::move (capture)),
        _descriptor (std::move (descriptor))
    {
    }

    void Record (T value, const opentelemetry::context::Context &) noexcept override
    {
        record (value, empty_attributes ());
    }

    void Record (T value,
                 const opentelemetry::common::KeyValueIterable &attributes,
                 const opentelemetry::context::Context &) noexcept override
    {
        record (value, attributes);
    }

  private:
    static const opentelemetry::common::NoopKeyValueIterable &empty_attributes ()
    {
        static const opentelemetry::common::NoopKeyValueIterable value;
        return value;
    }

    void record (T value, const opentelemetry::common::KeyValueIterable &attributes) noexcept
    {
        _capture->record_sync (_descriptor, static_cast<double> (value), attributes);
    }

    std::shared_ptr<evidence_metric_capture_t> _capture;
    metric_descriptor_t _descriptor;
};

template <typename T>
class evidence_observable_instrument_t final : public opentelemetry::metrics::ObservableInstrument,
                                               public evidence_observable_t
{
  public:
    evidence_observable_instrument_t (std::shared_ptr<evidence_metric_capture_t> capture,
                                      metric_descriptor_t descriptor,
                                      void *meter,
                                      void (*add) (void *, evidence_observable_t *) noexcept,
                                      void (*remove) (void *, evidence_observable_t *) noexcept) :
        _capture (std::move (capture)),
        _descriptor (std::move (descriptor)),
        _meter (meter),
        _remove (remove)
    {
        add (_meter, this);
    }

    ~evidence_observable_instrument_t () override { _remove (_meter, this); }

    void AddCallback (opentelemetry::metrics::ObservableCallbackPtr callback,
                      void *state) noexcept override
    {
        std::lock_guard lock (_mutex);
        _callbacks.push_back ({callback, state});
    }

    void RemoveCallback (opentelemetry::metrics::ObservableCallbackPtr callback,
                         void *state) noexcept override
    {
        std::lock_guard lock (_mutex);
        _callbacks.erase (std::remove_if (_callbacks.begin (), _callbacks.end (),
                                          [&] (const auto &entry) {
                                              return entry.callback == callback
                                                     && entry.state == state;
                                          }),
                          _callbacks.end ());
    }

    void collect () noexcept override
    {
        std::lock_guard lock (_mutex);
        for (const auto &entry : _callbacks) {
            opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObserverResultT<T>> observer (
              new observer_t (_capture, _descriptor));
            entry.callback (opentelemetry::metrics::ObserverResult{std::move (observer)},
                            entry.state);
        }
    }

  private:
    class observer_t final : public opentelemetry::metrics::ObserverResultT<T>
    {
      public:
        observer_t (std::shared_ptr<evidence_metric_capture_t> capture,
                    metric_descriptor_t descriptor) :
            _capture (std::move (capture)), _descriptor (std::move (descriptor))
        {
        }

        void Observe (T value) noexcept override
        {
            _capture->record_observable (_descriptor, static_cast<double> (value),
                                         empty_attributes ());
        }

        void Observe (T value,
                      const opentelemetry::common::KeyValueIterable &attributes) noexcept override
        {
            _capture->record_observable (_descriptor, static_cast<double> (value), attributes);
        }

      private:
        static const opentelemetry::common::NoopKeyValueIterable &empty_attributes ()
        {
            static const opentelemetry::common::NoopKeyValueIterable value;
            return value;
        }

        std::shared_ptr<evidence_metric_capture_t> _capture;
        metric_descriptor_t _descriptor;
    };

    struct callback_t
    {
        opentelemetry::metrics::ObservableCallbackPtr callback;
        void *state;
    };

    std::shared_ptr<evidence_metric_capture_t> _capture;
    metric_descriptor_t _descriptor;
    void *_meter;
    void (*_remove) (void *, evidence_observable_t *) noexcept;
    std::mutex _mutex;
    std::vector<callback_t> _callbacks;
};

class evidence_meter_t final : public opentelemetry::metrics::Meter
{
  public:
    explicit evidence_meter_t (std::shared_ptr<evidence_metric_capture_t> capture) :
        _capture (std::move (capture))
    {
    }

    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<std::uint64_t>>
    CreateUInt64Counter (opentelemetry::nostd::string_view name,
                         opentelemetry::nostd::string_view,
                         opentelemetry::nostd::string_view unit) noexcept override
    {
        return make_counter<std::uint64_t> (name, unit);
    }

    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<double>>
    CreateDoubleCounter (opentelemetry::nostd::string_view name,
                         opentelemetry::nostd::string_view,
                         opentelemetry::nostd::string_view unit) noexcept override
    {
        return make_counter<double> (name, unit);
    }

    opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObservableInstrument>
    CreateInt64ObservableCounter (opentelemetry::nostd::string_view name,
                                  opentelemetry::nostd::string_view,
                                  opentelemetry::nostd::string_view unit) noexcept override
    {
        return make_observable<std::int64_t> (name, unit, "counter", "cumulative");
    }

    opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObservableInstrument>
    CreateDoubleObservableCounter (opentelemetry::nostd::string_view name,
                                   opentelemetry::nostd::string_view,
                                   opentelemetry::nostd::string_view unit) noexcept override
    {
        return make_observable<double> (name, unit, "counter", "cumulative");
    }

    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Histogram<std::uint64_t>>
    CreateUInt64Histogram (opentelemetry::nostd::string_view name,
                           opentelemetry::nostd::string_view,
                           opentelemetry::nostd::string_view unit) noexcept override
    {
        return make_histogram<std::uint64_t> (name, unit);
    }

    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Histogram<double>>
    CreateDoubleHistogram (opentelemetry::nostd::string_view name,
                           opentelemetry::nostd::string_view,
                           opentelemetry::nostd::string_view unit) noexcept override
    {
        return make_histogram<double> (name, unit);
    }

    opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObservableInstrument>
    CreateInt64ObservableGauge (opentelemetry::nostd::string_view name,
                                opentelemetry::nostd::string_view,
                                opentelemetry::nostd::string_view unit) noexcept override
    {
        return make_observable<std::int64_t> (name, unit, "observable", "current");
    }

    opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObservableInstrument>
    CreateDoubleObservableGauge (opentelemetry::nostd::string_view name,
                                 opentelemetry::nostd::string_view,
                                 opentelemetry::nostd::string_view unit) noexcept override
    {
        return make_observable<double> (name, unit, "observable", "current");
    }

    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::UpDownCounter<std::int64_t>>
    CreateInt64UpDownCounter (opentelemetry::nostd::string_view name,
                              opentelemetry::nostd::string_view,
                              opentelemetry::nostd::string_view unit) noexcept override
    {
        return make_updown<std::int64_t> (name, unit);
    }

    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::UpDownCounter<double>>
    CreateDoubleUpDownCounter (opentelemetry::nostd::string_view name,
                               opentelemetry::nostd::string_view,
                               opentelemetry::nostd::string_view unit) noexcept override
    {
        return make_updown<double> (name, unit);
    }

    opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObservableInstrument>
    CreateInt64ObservableUpDownCounter (opentelemetry::nostd::string_view name,
                                        opentelemetry::nostd::string_view,
                                        opentelemetry::nostd::string_view unit) noexcept override
    {
        return make_observable<std::int64_t> (name, unit, "updown", "current");
    }

    opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObservableInstrument>
    CreateDoubleObservableUpDownCounter (opentelemetry::nostd::string_view name,
                                         opentelemetry::nostd::string_view,
                                         opentelemetry::nostd::string_view unit) noexcept override
    {
        return make_observable<double> (name, unit, "updown", "current");
    }

    void collect_observables () noexcept
    {
        _capture->begin_observable_collection ();
        std::lock_guard lock (_mutex);
        for (auto *instrument : _observables)
            instrument->collect ();
    }

  private:
    static metric_descriptor_t descriptor (opentelemetry::nostd::string_view name,
                                           opentelemetry::nostd::string_view unit,
                                           std::string kind,
                                           std::string temporality)
    {
        return {std::string (name.data (), name.size ()), std::string (unit.data (), unit.size ()),
                std::move (kind), std::move (temporality)};
    }

    template <typename T>
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<T>>
    make_counter (opentelemetry::nostd::string_view name,
                  opentelemetry::nostd::string_view unit) noexcept
    {
        return opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<T>> (
          new evidence_add_t<T, opentelemetry::metrics::NoopCounter> (
            _capture, descriptor (name, unit, "counter", "delta")));
    }

    template <typename T>
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Histogram<T>>
    make_histogram (opentelemetry::nostd::string_view name,
                    opentelemetry::nostd::string_view unit) noexcept
    {
        return opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Histogram<T>> (
          new evidence_histogram_t<T> (_capture, descriptor (name, unit, "histogram", "sample")));
    }

    template <typename T>
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::UpDownCounter<T>>
    make_updown (opentelemetry::nostd::string_view name,
                 opentelemetry::nostd::string_view unit) noexcept
    {
        return opentelemetry::nostd::unique_ptr<opentelemetry::metrics::UpDownCounter<T>> (
          new evidence_add_t<T, opentelemetry::metrics::NoopUpDownCounter> (
            _capture, descriptor (name, unit, "updown", "delta")));
    }

    template <typename T>
    opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObservableInstrument>
    make_observable (opentelemetry::nostd::string_view name,
                     opentelemetry::nostd::string_view unit,
                     std::string kind,
                     std::string temporality) noexcept
    {
        return opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObservableInstrument> (
          new evidence_observable_instrument_t<T> (
            _capture, descriptor (name, unit, std::move (kind), std::move (temporality)), this,
            &add_observable, &remove_observable));
    }

    static void add_observable (void *meter, evidence_observable_t *instrument) noexcept
    {
        auto &self = *static_cast<evidence_meter_t *> (meter);
        std::lock_guard lock (self._mutex);
        self._observables.push_back (instrument);
    }

    static void remove_observable (void *meter, evidence_observable_t *instrument) noexcept
    {
        auto &self = *static_cast<evidence_meter_t *> (meter);
        std::lock_guard lock (self._mutex);
        self._observables.erase (
          std::remove (self._observables.begin (), self._observables.end (), instrument),
          self._observables.end ());
    }

    std::shared_ptr<evidence_metric_capture_t> _capture;
    std::mutex _mutex;
    std::vector<evidence_observable_t *> _observables;
};

class evidence_meter_provider_t final : public opentelemetry::metrics::MeterProvider
{
  public:
    explicit evidence_meter_provider_t (std::shared_ptr<evidence_metric_capture_t> capture) :
        _meter (new evidence_meter_t (std::move (capture)))
    {
    }

    opentelemetry::nostd::shared_ptr<opentelemetry::metrics::Meter>
    GetMeter (opentelemetry::nostd::string_view,
              opentelemetry::nostd::string_view,
              opentelemetry::nostd::string_view) noexcept override
    {
        return _meter;
    }

    void collect_observables () noexcept
    {
        static_cast<evidence_meter_t *> (_meter.get ())->collect_observables ();
    }

  private:
    opentelemetry::nostd::shared_ptr<opentelemetry::metrics::Meter> _meter;
};

class evidence_metric_reader_t
{
  public:
    evidence_metric_reader_t () :
        _previous_provider (opentelemetry::metrics::Provider::GetMeterProvider ()),
        _capture (std::make_shared<evidence_metric_capture_t> ()),
        _provider (std::make_shared<evidence_meter_provider_t> (_capture))
    {
        opentelemetry::metrics::Provider::SetMeterProvider (
          opentelemetry::nostd::shared_ptr<opentelemetry::metrics::MeterProvider> (
            std::static_pointer_cast<opentelemetry::metrics::MeterProvider> (_provider)));
    }

    ~evidence_metric_reader_t ()
    {
        opentelemetry::metrics::Provider::SetMeterProvider (_previous_provider);
    }

    evidence_metric_reader_t (const evidence_metric_reader_t &) = delete;
    evidence_metric_reader_t &operator= (const evidence_metric_reader_t &) = delete;

    nlohmann::json collect () const
    {
        _provider->collect_observables ();
        return _capture->snapshot ();
    }

  private:
    opentelemetry::nostd::shared_ptr<opentelemetry::metrics::MeterProvider> _previous_provider;
    std::shared_ptr<evidence_metric_capture_t> _capture;
    std::shared_ptr<evidence_meter_provider_t> _provider;
};

/* Aggregates the config-11 §3 evidence arrays. Synchronous instruments retain
 * their actual samples while each evidence request refreshes observables. */
class observability_evidence_t
{
  public:
    explicit observability_evidence_t (const evidence_metric_reader_t *metric_reader) :
        _metric_reader (metric_reader)
    {
    }

    nlohmann::json snapshot () const
    {
        std::lock_guard lock (_mutex);
        return nlohmann::json{
          {"metrics", _metric_reader ? _metric_reader->collect () : nlohmann::json::array ()},
          {"drainEvents", _drain_events}};
    }

  private:
    const evidence_metric_reader_t *_metric_reader;
    mutable std::mutex _mutex;
    std::vector<nlohmann::json> _drain_events;
};

/* Runtime control seam for the HTTP handlers: bound to the app after
 * create() so the drain endpoints reach the shared drain operation. */
struct drain_control_t
{
    std::function<void (std::chrono::milliseconds)> start_drain;
    std::function<bool ()> is_ready;
};

struct host_role_descriptor_t
{
    zlink::framework::e2e::observability_ops::server::host_role_t value;
};

class room_spot_t;

class workflow_event_store_t
{
  public:
    workflow_event_store_t (std::filesystem::path directory, std::string writer_id) :
        _directory (std::move (directory)), _writer_id (std::move (writer_id))
    {
        std::filesystem::create_directories (_directory);
    }

    std::optional<int> replay (const std::string &spot_id) const
    {
        std::ifstream input (path_for (spot_id));
        int event_value = 0;
        int applied = 0;
        bool found = false;
        while (input >> event_value) {
            applied += event_value;
            found = true;
        }
        return found ? std::optional<int> (applied) : std::nullopt;
    }

    void append (const std::string &spot_id, int event_value) const
    {
        std::ofstream output (path_for (spot_id), std::ios::app);
        if (!(output << event_value << '\n')) {
            throw std::runtime_error ("workflow event append failed for " + _writer_id);
        }
    }

  private:
    std::filesystem::path path_for (const std::string &spot_id) const
    {
        std::string encoded;
        static constexpr char digits[] = "0123456789abcdef";
        encoded.reserve (spot_id.size () * 2);
        for (const auto value : spot_id) {
            const auto byte = static_cast<unsigned char> (value);
            encoded.push_back (digits[byte >> 4]);
            encoded.push_back (digits[byte & 0x0f]);
        }
        return _directory / (encoded + ".state");
    }

    std::filesystem::path _directory;
    std::string _writer_id;
};

/* OBS-A4(b): timer-originated callbacks start a fresh flow (origin=timer);
 * the tick publishes so the fresh flow shows up as a `sent` line. */
struct room_timer_handler_t
{
    void handle (room_spot_t &spot, const fw::timer_tick_t &tick) const;
};

class room_spot_t : public fw::spot_t<fw::actor_t>
{
  public:
    room_spot_t (fw::spot_context_t context,
                 bool timer_enabled,
                 std::shared_ptr<workflow_event_store_t> event_store = {}) :
        _context (std::move (context)),
        _event_store (std::move (event_store)),
        _timer_enabled (timer_enabled)
    {
    }

    fw::spot_context_t &context () noexcept override { return _context; }
    const fw::spot_context_t &context () const noexcept override { return _context; }

    void configure () override
    {
        _context.handlers ().add_handler<&room_spot_t::apply_action> (
          obs::obs_action_req_t::packet_name);
        _context.handlers ().add_subscribe<&room_spot_t::on_projection> (
          obs::projection_topic);
    }

    fw::task_t<void> on_initialize () override
    {
        if (_event_store) {
            _applied = _event_store
                         ->replay (_context.spot_id ())
                         .value_or (0);
        }
        if (_timer_enabled) {
            _timer = _context.add_timer<room_timer_handler_t> ("obs-tick",
                                                               std::chrono::milliseconds (200));
        }
        co_return;
    }

    fw::task_t<fw::spot_actor_join_result_t>
    on_actor_join (std::string_view, const fw::message_t &) override
    {
        co_return fw::spot_actor_join_result_t::accept ();
    }

    fw::task_t<void> on_actor_joined (fw::actor_t &) override { co_return; }
    fw::task_t<void> on_leave_actor (fw::actor_t &) override { co_return; }

    obs::obs_action_res_t apply_action (const obs::obs_action_req_t &request)
    {
        if (_event_store) {
            _event_store->append (request.spot_id, request.value);
        }
        _applied += request.value;
        /* OBS-A4(a)/B3: one action fans out to every mesh subscriber under
         * the same flow. */
        _context
          .publish (obs::projection_topic,
                    obs::projection_event_t{request.spot_id, request.marker, _applied})
          .async ();
        return obs::obs_action_res_t{request.spot_id, request.marker, _applied};
    }

    void on_projection (const obs::projection_event_t &) { ++_projections_seen; }

    void publish_timer_marker ()
    {
        _context
          .publish (obs::projection_topic,
                    obs::projection_event_t{_context.spot_id (),
                                            "obs-timer", _applied})
          .async ();
    }

  private:
    fw::spot_context_t _context;
    fw::timer_t _timer;
    std::shared_ptr<workflow_event_store_t> _event_store;
    int _applied = 0;
    int _projections_seen = 0;
    bool _timer_enabled;
};

void room_timer_handler_t::handle (room_spot_t &spot, const fw::timer_tick_t &) const
{
    spot.publish_timer_marker ();
}

struct obs_actor_t : fw::actor_t
{
    explicit obs_actor_t (fw::actor_context_t value) :
        actor_id (value.actor_ref ().actor_id ().value ()),
        actor_ref (value.actor_ref ()),
        actor_context (std::move (value))
    {
    }
    fw::actor_context_t &context () noexcept override { return actor_context; }
    const fw::actor_context_t &context () const noexcept override { return actor_context; }

    std::string actor_id;
    int total = 0;
    fw::actor_ref_t actor_ref;
    fw::actor_context_t actor_context;
};

struct obs_actor_factory_t final : fw::actor_factory_t<obs_actor_t>
{
    fw::task_t<std::shared_ptr<obs_actor_t>>
    create (fw::actor_context_t context, std::stop_token) override
    {
        co_return std::make_shared<obs_actor_t> (std::move (context));
    }
};

/* Entry spot hosts the player actors: admission accepts every join and the
 * ping handler accumulates state, so post-transfer pings prove continuity. */
class obs_entry_spot_t : public fw::entry_spot_t<obs_actor_t>
{
  public:
    explicit obs_entry_spot_t (fw::entry_spot_context_t context) :
        _context (std::move (context))
    {
    }

    fw::entry_spot_context_t &context () noexcept override { return _context; }
    const fw::entry_spot_context_t &context () const noexcept override
    {
        return _context;
    }

    void configure () override
    {
        _context.handlers ().add_actor_request<&obs_entry_spot_t::actor_ping> (
          obs::actor_ping_req_t::packet_name);
    }

    fw::task_t<fw::spot_actor_join_result_t>
    on_actor_join (std::string_view actor_id, const fw::message_t &) override
    {
        co_return fw::spot_actor_join_result_t::accept (
          obs::join_actor_res_t{std::string (actor_id),
                                std::string (_context.node_rid ().value ()), true, {}});
    }

    fw::task_t<void> on_actor_joined (obs_actor_t &) override { co_return; }
    fw::task_t<void> on_leave_actor (obs_actor_t &) override { co_return; }

    obs::actor_ping_res_t actor_ping (obs_actor_t &actor,
                                      fw::message_context_t &,
                                      const obs::actor_ping_req_t &request)
    {
        actor.total += request.value;
        return obs::actor_ping_res_t{actor.actor_id,
                                     std::string (_context.node_rid ().value ()), actor.total};
    }

  private:
    fw::entry_spot_context_t _context;
};

class join_actor_handler_t
{
  public:
    using request_type = obs::join_actor_req_t;
    using reply_type = obs::join_actor_res_t;

    join_actor_handler_t (fw::session_actor_manager_t &actors,
                          server_options_t &options) :
        _actors (actors), _node_rid (options.node_rid)
    {
    }

    obs::join_actor_res_t handle (const obs::join_actor_req_t &request)
    {
        try {
            auto actor = _actors.get_or_create (obs::actor_type, request.actor_id);
            if (!actor) {
                const auto *error = actor.error ();
                return obs::join_actor_res_t{request.actor_id, {}, false,
                                             error != nullptr ? error->what () : "join failed"};
            }
            return obs::join_actor_res_t{
              request.actor_id,
              std::string (actor.value ().ref ().node_rid ().value ()),
              true,
              {}};
        }
        catch (const std::exception &error) {
            return obs::join_actor_res_t{request.actor_id, {}, false, error.what ()};
        }
    }

  private:
    fw::session_actor_manager_t &_actors;
    std::string _node_rid;
};

class actor_ping_handler_t
{
  public:
    using request_type = obs::actor_ping_req_t;
    using reply_type = obs::actor_ping_res_t;

    actor_ping_handler_t (fw::actor_client_t &actors, fw::actor_directory_t &directory) :
        _actors (actors), _directory (directory)
    {
    }

    fw::task_t<obs::actor_ping_res_t> handle (const obs::actor_ping_req_t &request)
    {
        auto actor_ref = co_await _directory.find (request.actor_id);
        if (!actor_ref) {
            throw fw::framework_exception_t (fw::framework_error_kind_t::not_found,
                                             "player actor route was not found");
        }
        co_return co_await _actors.request (fw::actor_id_t (request.actor_id), request)
          .timeout (std::chrono::milliseconds (5000))
          .async<obs::actor_ping_res_t> ();
    }

  private:
    fw::actor_client_t &_actors;
    fw::actor_directory_t &_directory;
};

/* play-a session gateway: forwards ObsActionReq packets to the target room
 * spot through the opaque spot handle (flow-correlation OBS-A1 chain:
 * connector -> stream inbound -> spot dispatch). */
class obs_session_t final : public fw::packet_stream_session_t
{
  public:

    explicit obs_session_t (fw::route_client_t &routes) :
        _routes (routes)
    {
    }

    fw::task_t<void> on_connected (fw::stream_t &) override { co_return; }
    fw::task_t<void> on_disconnected (fw::stream_t &) override { co_return; }
    fw::task_t<void> on_error (fw::stream_t &, const fw::stream_error_t &) override { co_return; }

    fw::task_t<void> on_packet (fw::stream_t &stream,
                                const fw::session_message_context_t &dispatch,
                                const zlink::message_t &payload) override
    {
        if (dispatch.packet_name != obs::obs_action_req_t::packet_name) {
            throw fw::framework_exception_t (fw::framework_error_kind_t::not_found,
                                             "ObservabilityOps session has no handler for "
                                               + std::string (dispatch.packet_name));
        }
        auto request = payload.parse_json<obs::obs_action_req_t> ();
        auto reply = co_await _routes.request_to_spot (request.spot_id, request)
                       .timeout (std::chrono::milliseconds (5000))
                       .async<obs::obs_action_res_t> ();
        stream.reply_packet (zlink::message_t::from_json (reply))
          .async ();
        co_return;
    }

  private:
    fw::route_client_t &_routes;
};

class evidence_handler_t
{
  public:

    evidence_handler_t (observability_evidence_t &evidence,
                        fw::location_runtime_query_t &locations,
                        fw::route_mesh_runtime_t &route_mesh,
                        drain_control_t &drain) :
        _evidence (evidence), _locations (locations),
        _route_mesh (route_mesh), _drain (drain)
    {
    }

    fw::http_response_t handle (const fw::http_request_t &)
    {
        nlohmann::json json;
        try {
            json = _evidence.snapshot ();
        }
        catch (const std::exception &error) {
            json = nlohmann::json{{"metrics", nlohmann::json::array ()},
                                  {"drainEvents", nlohmann::json::array ()},
                                  {"snapshotError", error.what ()}};
        }
        auto peer_rows = nlohmann::json::array ();
        try {
            const auto snapshot = _route_mesh.snapshot ("play");
            for (const auto &peer : snapshot.peers) {
                peer_rows.push_back (nlohmann::json{
                  {"nodeRid", peer.node_rid.to_hex ()},
                  {"meshName", snapshot.mesh_name},
                  {"draining", peer.state == fw::peer_state_t::draining}});
            }
        }
        catch (const std::exception &error) {
            json["peerRowsError"] = error.what ();
        }
        json["peerRows"] = peer_rows;
        auto owner_leases = nlohmann::json::array ();
        try {
            const auto status = _locations.get_status ().result ().value ();
            nlohmann::json lease{
              {"healthy", status.owner_lease_healthy},
              {"renewedAtUnixMs", nullptr}};
            if (status.owner_lease_renewed_at) {
                lease["renewedAtUnixMs"] =
                  std::chrono::duration_cast<std::chrono::milliseconds> (
                    status.owner_lease_renewed_at->time_since_epoch ())
                    .count ();
            }
            owner_leases.push_back (std::move (lease));
        }
        catch (const std::exception &error) {
            json["ownerLeasesError"] = error.what ();
        }
        json["ownerLeases"] = std::move (owner_leases);
        try {
            json["ready"] = _drain.is_ready ? _drain.is_ready () : true;
        }
        catch (const std::exception &error) {
            json["readyError"] = error.what ();
            json["ready"] = true;
        }
        fw::http_response_t response;
        /* Owner ids/rids may contain raw bytes; evidence is diagnostic, so
         * non-UTF-8 sequences are replaced instead of failing the endpoint. */
        response.body =
          json.dump (-1, ' ', false, nlohmann::json::error_handler_t::replace);
        return response;
    }

  private:
    observability_evidence_t &_evidence;
    fw::location_runtime_query_t &_locations;
    fw::route_mesh_runtime_t &_route_mesh;
    drain_control_t &_drain;
};

class action_handler_t
{
  public:
    using request_type = obs::obs_action_req_t;
    using reply_type = obs::obs_action_res_t;

    explicit action_handler_t (fw::route_client_t &routes) :
        _routes (routes)
    {
    }

    fw::task_t<obs::obs_action_res_t> handle (const obs::obs_action_req_t &request)
    {
        co_return co_await _routes.request_to_spot (request.spot_id, request)
          .timeout (std::chrono::milliseconds (5000))
          .async<obs::obs_action_res_t> ();
    }

  private:
    fw::route_client_t &_routes;
};

class create_room_handler_t
{
  public:
    using request_type = obs::create_room_req_t;
    using reply_type = obs::create_room_res_t;

    create_room_handler_t (fw::spot_manager_t &spots,
                           drain_control_t &drain,
                           host_role_descriptor_t &role) :
        _spots (spots), _drain (drain), _role (role)
    {
    }

    obs::create_room_res_t handle (const obs::create_room_req_t &request)
    {
        if (_drain.is_ready && !_drain.is_ready ()) {
            return obs::create_room_res_t{request.spot_id, "rejected"};
        }
        const auto created =
          _spots
            .get_or_create (
              request.spot_id,
              _role.value ==
                  zlink::framework::e2e::observability_ops::server::host_role_t::order_workflow
                ? obs::order_workflow_spot
                : obs::room_spot)
            .async ()
            .result ();
        if (!created) {
            return obs::create_room_res_t{request.spot_id, "rejected"};
        }
        const auto state =
          created.value ().state == fw::spot_create_state_t::existing
            ? "existing"
            : "created";
        return obs::create_room_res_t{request.spot_id, state};
    }

  private:
    fw::spot_manager_t &_spots;
    drain_control_t &_drain;
    host_role_descriptor_t &_role;
};

class close_room_handler_t
{
  public:
    using request_type = obs::create_room_req_t;
    using reply_type = obs::create_room_res_t;

    explicit close_room_handler_t (fw::spot_manager_t &spots) : _spots (spots) {}

    obs::create_room_res_t handle (const obs::create_room_req_t &request)
    {
        const auto found = _spots.find (request.spot_id).result ();
        if (!found || !found.value ()) {
            return obs::create_room_res_t{request.spot_id, "not_closed"};
        }
        const auto closed = _spots.close (*found.value ()).result ();
        return obs::create_room_res_t{
          request.spot_id, closed && closed.value () ? "closed" : "not_closed"};
    }

  private:
    fw::spot_manager_t &_spots;
};

class drain_handler_t
{
  public:
    using request_type = obs::drain_req_t;
    using reply_type = obs::create_room_res_t;

    explicit drain_handler_t (drain_control_t &drain) : _drain (drain) {}

    obs::create_room_res_t handle (const obs::drain_req_t &request)
    {
        if (_drain.start_drain) {
            _drain.start_drain (std::chrono::milliseconds (request.deadline_ms));
        }
        return obs::create_room_res_t{"", "draining"};
    }

  private:
    drain_control_t &_drain;
};

} // namespace

int zlink::framework::e2e::observability_ops::server::run_host (host_role_t role,
                                                                int argc,
                                                                char **argv)
{
    fw::config_builder_t config;
    config.load_cli (argc, argv);
    const auto config_path = config.model ().get ("config");
    if (!config_path) {
        throw std::runtime_error ("ObservabilityOps server requires --config=<path>");
    }
    config.load_json (*config_path);
    const auto options = config.bind_required<server_options_t> ("e2e");
    if (role == host_role_t::session && options.stream_endpoint.empty ()) {
        throw std::runtime_error ("ObservabilityOps Session requires streamEndpoint");
    }
    if (role != host_role_t::session && !options.stream_endpoint.empty ()) {
        throw std::runtime_error ("only ObservabilityOps Session accepts streamEndpoint");
    }
    if (options.spot_router_endpoint.empty () || options.spot_pub_endpoint.empty ()) {
        throw std::runtime_error ("ObservabilityOps Spot host requires router and pub endpoints");
    }
    std::optional<evidence_metric_reader_t> metric_reader;
    if (options.metrics_enabled) {
        metric_reader.emplace ();
    }
    auto app = fw::app_t::create ();
    app.config () = std::move (config);
    auto evidence_owner =
      std::make_unique<observability_evidence_t> (metric_reader ? &*metric_reader : nullptr);
    auto drain_control_owner = std::make_unique<drain_control_t> ();
    auto *drain_control = drain_control_owner.get ();
    drain_control->start_drain = [&app] (std::chrono::milliseconds deadline) {
        (void) app.shutdown (deadline);
    };
    drain_control->is_ready = [&app] { return app.is_ready (); };

    const auto workflow_events = role == host_role_t::order_workflow
                                   ? std::make_shared<workflow_event_store_t> (
                                       std::filesystem::path (options.log_dir)
                                         / "workflow-events",
                                       options.node_rid)
                                   : std::shared_ptr<workflow_event_store_t>{};
    app.logging ().use_file (
      options.log_dir + "/" + options.node_rid + "-flow.log");

    app.add_zlink_framework ([&] (fw::zlink_framework_options_t &framework) {
        framework.services ().add_singleton<server_options_t> (
          std::make_unique<server_options_t> (options));
        framework.services ().add_singleton<observability_evidence_t> (
          std::move (evidence_owner));
        framework.services ().add_singleton<drain_control_t> (std::move (drain_control_owner));
        framework.services ().add_singleton<host_role_descriptor_t> (
          std::make_unique<host_role_descriptor_t> (host_role_descriptor_t{role}));
        framework.add_location_store (
          std::make_shared<fw::redis::redis_location_store_t> (
            fw::redis::redis_location_options_t{
              .connection_string = options.redis_endpoint,
              .key_prefix = options.redis_key_prefix}));
        {
            auto &locations = framework.configure_locations ();
            locations.owner_lease_renew_interval = std::chrono::seconds (1);
            locations.owner_lease_ttl = std::chrono::seconds (15);
            locations.polling_interval = std::chrono::milliseconds (250);
        }
        /* OBS-A3(b): an off node must not create flows but still propagates
         * the received pair to the next hop. */
        framework.configure_dispatch ()
          .message_flow (options.trace_mode == "off" ? fw::message_flow_log_mode_t::off
                                                     : fw::message_flow_log_mode_t::normal);

        if (role == host_role_t::session) {
            auto spot = framework.add_route_mesh (obs::spot_mesh);
            spot.channel_name (obs::spot_mesh).server ();
            spot.set_routing_id (zlink::routing_id_t::from (options.node_rid))
              .listen (options.spot_router_endpoint);
        } else {
            const auto *mesh_name = role == host_role_t::order_workflow
                                      ? obs::workflow_spot_mesh
                                      : obs::spot_mesh;
            const auto *spot_type = role == host_role_t::order_workflow
                                      ? obs::order_workflow_spot
                                      : obs::room_spot;
            if (role == host_role_t::play) {
                auto spot = framework.add_route_mesh (mesh_name);
                spot.channel_name (mesh_name).server ();
                spot.set_routing_id (zlink::routing_id_t::from (options.node_rid))
                  .listen (options.spot_router_endpoint)
                  .add_entry_spot<obs_entry_spot_t> (
                    [] (fw::entry_spot_context_t context) {
                        return std::make_shared<obs_entry_spot_t> (
                          std::move (context));
                    })
                  .add_spot_factory<room_spot_t> (
                    spot_type,
                    [timer_enabled = options.room_timer_enabled] (
                      fw::spot_context_t context) {
                        return std::make_shared<room_spot_t> (
                          std::move (context), timer_enabled);
                    },
                    [] (auto &factory) {
                        factory.disable_relocation ();
                    })
                  .add_actor_factory<obs_actor_t, obs_actor_factory_t> (
                    obs::actor_type,
                    std::make_shared<obs_actor_factory_t> (),
                    [] (auto &factory) {
                        factory.disable_relocation ();
                    });
            } else {
                auto spot = framework.add_route_mesh (mesh_name);
                spot.channel_name (mesh_name).server ();
                spot.set_routing_id (zlink::routing_id_t::from (options.node_rid))
                  .listen (options.spot_router_endpoint)
                  .add_spot_factory<room_spot_t> (
                    spot_type,
                    [timer_enabled = options.room_timer_enabled,
                     workflow_events] (fw::spot_context_t context) {
                        return std::make_shared<room_spot_t> (
                          std::move (context), timer_enabled,
                          workflow_events);
                    },
                    [] (auto &factory) {
                        factory.disable_relocation ();
                    });
            }
        }

        if (role == host_role_t::session) {
            framework.add_stream_node ("obs.session")
              .bind (options.stream_endpoint)
              .register_session<obs_session_t> ();
        }

        auto &http = framework.http ().listen (options.http_endpoint)
          .map_health ("/health")
          .map_get<evidence_handler_t> ("/evidence")
          .map_post<drain_handler_t> ("/drain");
        if (role != host_role_t::session) {
            http.map_post<action_handler_t> ("/spot/action")
              .map_post<create_room_handler_t> ("/spot/create")
              .map_post<close_room_handler_t> ("/spot/close");
        }
        if (role == host_role_t::play) {
            http.map_post<join_actor_handler_t> ("/actor/join")
              .map_post<actor_ping_handler_t> ("/actor/ping");
        }
    });
    return app.run (argc, argv);
}

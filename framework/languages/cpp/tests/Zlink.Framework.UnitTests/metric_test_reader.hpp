/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <opentelemetry/metrics/provider.h>
#include <opentelemetry/sdk/metrics/meter_provider.h>
#include <opentelemetry/sdk/metrics/metric_reader.h>

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace metric_test
{
namespace sdk = opentelemetry::sdk::metrics;

class reader_t final : public sdk::MetricReader
{
  public:
    sdk::AggregationTemporality
    GetAggregationTemporality (sdk::InstrumentType) const noexcept override
    {
        return sdk::AggregationTemporality::kCumulative;
    }

  private:
    bool OnForceFlush (std::chrono::microseconds) noexcept override { return true; }
    bool OnShutDown (std::chrono::microseconds) noexcept override { return true; }
};

class provider_t
{
  public:
    provider_t () :
        _previous (opentelemetry::metrics::Provider::GetMeterProvider ()),
        _provider (std::make_shared<sdk::MeterProvider> ()),
        reader (std::make_shared<reader_t> ())
    {
        _provider->AddMetricReader (reader);
        opentelemetry::metrics::Provider::SetMeterProvider (
          opentelemetry::nostd::shared_ptr<opentelemetry::metrics::MeterProvider> (
            std::static_pointer_cast<opentelemetry::metrics::MeterProvider> (_provider)));
    }

    ~provider_t () { opentelemetry::metrics::Provider::SetMeterProvider (_previous); }

    std::vector<sdk::MetricData> collect ()
    {
        std::vector<sdk::MetricData> result;
        if (!reader->Collect ([&] (sdk::ResourceMetrics &resource) {
                for (const auto &scope : resource.scope_metric_data_)
                    result.insert (result.end (), scope.metric_data_.begin (),
                                   scope.metric_data_.end ());
                return true;
            }))
            throw std::runtime_error ("metric collection failed");
        return result;
    }

    std::vector<std::map<std::string, std::string>> collect_fields ()
    {
        std::vector<std::map<std::string, std::string>> result;
        for (const auto &metric : collect ()) {
            for (const auto &point : metric.point_data_attr_) {
                const auto *sum =
                  opentelemetry::nostd::get_if<sdk::SumPointData> (&point.point_data);
                if (!sum)
                    continue;
                auto &fields = result.emplace_back ();
                fields["name"] = metric.instrument_descriptor.name_;
                fields["unit"] = metric.instrument_descriptor.unit_;
                fields["instrument_kind"] = sum->is_monotonic_ ? "counter" : "updown";
                fields["temporality"] =
                  metric.aggregation_temporality == sdk::AggregationTemporality::kCumulative
                    ? "current"
                    : "delta";
                fields["value"] = std::to_string (opentelemetry::nostd::get<double> (sum->value_));
                for (const auto &[key, value] : point.attributes)
                    fields[key] = opentelemetry::nostd::get<std::string> (value);
            }
        }
        return result;
    }

  private:
    opentelemetry::nostd::shared_ptr<opentelemetry::metrics::MeterProvider> _previous;
    std::shared_ptr<sdk::MeterProvider> _provider;

  public:
    std::shared_ptr<reader_t> reader;
};
} // namespace metric_test

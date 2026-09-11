/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#ifndef ZLINK_CPP_BENCH_METRIC_READER_HPP
#define ZLINK_CPP_BENCH_METRIC_READER_HPP

#include <opentelemetry/metrics/provider.h>
#include <opentelemetry/nostd/variant.h>
#include <opentelemetry/sdk/metrics/data/metric_data.h>
#include <opentelemetry/sdk/metrics/meter_provider.h>
#include <opentelemetry/sdk/metrics/metric_reader.h>
#include <opentelemetry/sdk/metrics/provider.h>

#include <chrono>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace zlink_cpp_bench
{

class bench_metric_reader_t
{
  public:
    using label_t = std::pair<std::string_view, std::string_view>;

    bench_metric_reader_t () :
        _previous_provider (opentelemetry::metrics::Provider::GetMeterProvider ()),
        _provider (std::make_shared<opentelemetry::sdk::metrics::MeterProvider> ()),
        _reader (std::make_shared<manual_reader_t> ())
    {
        _provider->AddMetricReader (_reader);
        opentelemetry::sdk::metrics::Provider::SetMeterProvider (
          opentelemetry::nostd::shared_ptr<opentelemetry::metrics::MeterProvider> (
            std::static_pointer_cast<opentelemetry::metrics::MeterProvider> (_provider)));
    }

    ~bench_metric_reader_t ()
    {
        opentelemetry::metrics::Provider::SetMeterProvider (_previous_provider);
    }

    bench_metric_reader_t (const bench_metric_reader_t &) = delete;
    bench_metric_reader_t &operator= (const bench_metric_reader_t &) = delete;

    std::optional<double> collect_cumulative_sum (std::string_view metric_name,
                                                  std::initializer_list<label_t> labels) const
    {
        std::optional<double> value;
        _reader->Collect ([&] (opentelemetry::sdk::metrics::ResourceMetrics &resource_metrics) {
            for (const auto &scope_metrics : resource_metrics.scope_metric_data_) {
                for (const auto &metric : scope_metrics.metric_data_) {
                    if (metric.instrument_descriptor.name_ != metric_name)
                        continue;
                    for (const auto &point : metric.point_data_attr_) {
                        if (!has_labels (point.attributes, labels))
                            continue;
                        const auto *sum =
                          opentelemetry::nostd::get_if<opentelemetry::sdk::metrics::SumPointData> (
                            &point.point_data);
                        if (sum == nullptr)
                            continue;
                        const auto *observed = opentelemetry::nostd::get_if<double> (&sum->value_);
                        if (observed != nullptr)
                            value = *observed;
                    }
                }
            }
            return true;
        });
        return value;
    }

  private:
    class manual_reader_t final : public opentelemetry::sdk::metrics::MetricReader
    {
      public:
        opentelemetry::sdk::metrics::AggregationTemporality GetAggregationTemporality (
          opentelemetry::sdk::metrics::InstrumentType) const noexcept override
        {
            return opentelemetry::sdk::metrics::AggregationTemporality::kCumulative;
        }

      private:
        bool OnForceFlush (std::chrono::microseconds) noexcept override { return true; }
        bool OnShutDown (std::chrono::microseconds) noexcept override { return true; }
    };

    static bool has_labels (const opentelemetry::sdk::metrics::PointAttributes &attributes,
                            std::initializer_list<label_t> expected)
    {
        for (const auto &[name, value] : expected) {
            const auto attribute = attributes.find (std::string (name));
            if (attribute == attributes.end ())
                return false;
            const auto *actual = opentelemetry::nostd::get_if<std::string> (&attribute->second);
            if (actual == nullptr || *actual != value)
                return false;
        }
        return true;
    }

    opentelemetry::nostd::shared_ptr<opentelemetry::metrics::MeterProvider> _previous_provider;
    std::shared_ptr<opentelemetry::sdk::metrics::MeterProvider> _provider;
    std::shared_ptr<manual_reader_t> _reader;
};

} // namespace zlink_cpp_bench

#endif

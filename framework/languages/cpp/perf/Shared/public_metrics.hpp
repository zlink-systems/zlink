#pragma once
#include "contracts.hpp"
#include <opentelemetry/metrics/provider.h>
#include <opentelemetry/sdk/metrics/meter_provider.h>
#include <opentelemetry/sdk/metrics/metric_reader.h>
#include <mutex>

namespace perf {
namespace otel=opentelemetry;namespace om=opentelemetry::sdk::metrics;
class public_metric_reader_t final:public om::MetricReader {
  public:
    om::AggregationTemporality GetAggregationTemporality(om::InstrumentType) const noexcept override{return om::AggregationTemporality::kCumulative;}
    bool OnForceFlush(std::chrono::microseconds) noexcept override{return true;}bool OnShutDown(std::chrono::microseconds) noexcept override{return true;}
    json collect(){json result=json::array();const bool ok=Collect([&](om::ResourceMetrics &resource){for(const auto &scope:resource.scope_metric_data_)for(const auto &metric:scope.metric_data_)for(const auto &point:metric.point_data_attr_){json attributes=json::object();for(const auto &[key,value]:point.attributes)attributes[key]=otel::nostd::visit([](const auto &item)->json{return json(item);},value);const auto number=[](const om::ValueType &value)->json{return otel::nostd::visit([](const auto &item)->json{if constexpr(std::is_integral_v<std::decay_t<decltype(item)>>)return std::to_string(item);else return item;},value);};json value=nullptr;std::string type;
            if(const auto *sum=otel::nostd::get_if<om::SumPointData>(&point.point_data)){type="sum";value=number(sum->value_);}else if(const auto *gauge=otel::nostd::get_if<om::LastValuePointData>(&point.point_data)){type="lastValue";if(gauge->is_lastvalue_valid_)value=number(gauge->value_);}else if(const auto *hist=otel::nostd::get_if<om::HistogramPointData>(&point.point_data)){type="histogram";json counts=json::array();for(auto count:hist->counts_)counts.push_back(std::to_string(count));value={{"count",std::to_string(hist->count_)},{"sum",number(hist->sum_)},{"bounds",hist->boundaries_},{"counts",counts}};}else continue;
            result.push_back({{"name",metric.instrument_descriptor.name_},{"unit",metric.instrument_descriptor.unit_},{"type",type},{"value",value},{"attributes",attributes}});}return true;});if(!ok)throw std::runtime_error("OpenTelemetry public metric reader collection failed.");return result;}
};
class public_metrics_t {
    std::shared_ptr<om::MeterProvider> provider;
  public:
    std::shared_ptr<public_metric_reader_t> reader=std::make_shared<public_metric_reader_t>();
    public_metrics_t(){provider=std::make_shared<om::MeterProvider>();provider->AddMetricReader(reader);otel::metrics::Provider::SetMeterProvider(otel::nostd::shared_ptr<otel::metrics::MeterProvider>(provider));}
    ~public_metrics_t(){provider->Shutdown();}
};
}

/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once
#include "../Zlink.Framework.UnitTests/metric_test_reader.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>

namespace metric_test
{
inline const nlohmann::json &metric_contract ()
{
    static const auto fixture = [] {
        auto root =
          std::filesystem::weakly_canonical (std::filesystem::path (__FILE__)).parent_path ();
        for (unsigned i = 0; i != 4; ++i)
            root = root.parent_path ();
        std::ifstream input (root / "runtime/protocol/fixtures/runtime-metrics-v1.json");
        if (!input)
            throw std::runtime_error ("runtime metric fixture is missing");
        return nlohmann::json::parse (input).at ("metrics");
    }();
    return fixture;
}

inline void validate_metric_contract (const std::vector<sdk::MetricData> &data)
{
    const auto &fixture = metric_contract ();
    for (const auto &metric : data) {
        const auto &name = metric.instrument_descriptor.name_;
        const auto found = std::find_if (fixture.begin (), fixture.end (), [&] (const auto &entry) {
            return entry.at ("name") == name;
        });
        if (found == fixture.end ())
            throw std::runtime_error ("metric outside contract: " + name);
        const auto &contract = *found;
        if (contract.at ("unit") != metric.instrument_descriptor.unit_)
            throw std::runtime_error ("metric unit: " + name);
        for (const auto &point : metric.point_data_attr_) {
            std::string kind;
            if (const auto *sum =
                  opentelemetry::nostd::get_if<sdk::SumPointData> (&point.point_data))
                kind = sum->is_monotonic_ ? "counter" : "updown";
            else if (opentelemetry::nostd::get_if<sdk::HistogramPointData> (&point.point_data))
                kind = "histogram";
            else if (opentelemetry::nostd::get_if<sdk::LastValuePointData> (&point.point_data))
                kind = "observable";
            if (contract.at ("kind") != kind)
                throw std::runtime_error ("metric kind: " + name);
            const auto allowed = contract.at ("labels").template get<std::set<std::string>> ();
            const auto optional = contract.value ("optionalLabels", std::set<std::string>{});
            for (const auto &key : allowed)
                if (!optional.contains (key) && !point.attributes.contains (key))
                    throw std::runtime_error ("missing metric label: " + name + ":" + key);
            for (const auto &[key, value] : point.attributes) {
                if (!allowed.contains (key))
                    throw std::runtime_error ("metric label outside contract: " + name + ":" + key);
                if (contract.contains ("labelValues")
                    && contract.at ("labelValues").contains (key)) {
                    const auto &values = contract.at ("labelValues").at (key);
                    const auto &label = opentelemetry::nostd::get<std::string> (value);
                    if (std::find (values.begin (), values.end (), label) == values.end ())
                        throw std::runtime_error ("metric label value: " + name + ":" + key + "="
                                                  + label);
                }
            }
        }
    }
}
} // namespace metric_test

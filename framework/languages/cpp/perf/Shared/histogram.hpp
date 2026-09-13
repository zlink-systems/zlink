#pragma once
#include "contracts.hpp"
#include <algorithm>
#include <boost/multiprecision/cpp_int.hpp>
#include <map>

namespace perf {
struct histogram_t {
    const std::vector<std::uint64_t> &bounds;std::vector<std::uint64_t> counts; std::uint64_t count=0,overflow=0,max=0; boost::multiprecision::cpp_int sum=0;
    static const std::vector<std::uint64_t> &load_bounds(){static const auto value=read_json(std::string(ZLINK_PERF_CONTRACT_DIR)+"/histogram-bounds-ns.json").get<std::vector<std::uint64_t>>();return value;}
    histogram_t():bounds(load_bounds()),counts(bounds.size()) {}
    void record(std::int64_t ns) { if(ns<0) throw std::range_error("Negative elapsed.");const auto value=static_cast<std::uint64_t>(ns);auto at=std::lower_bound(bounds.begin(),bounds.end(),value)-bounds.begin();if(static_cast<std::size_t>(at)==bounds.size())++overflow;else++counts[at];++count;sum+=value;max=std::max(max,value); }
    json percentile(int numerator,int denominator=100) const { if(!count)return nullptr;const boost::multiprecision::cpp_int rank=(boost::multiprecision::cpp_int(numerator)*count+denominator-1)/denominator;boost::multiprecision::cpp_int cumulative=0;for(std::size_t i=0;i<counts.size();++i){cumulative+=counts[i];if(cumulative>=rank)return bounds[i]/1e6;}return nullptr; }
    json snapshot() const { json b=json::array(),c=json::array();for(auto x:bounds)b.push_back(std::to_string(x));for(auto x:counts)c.push_back(std::to_string(x));return {{"unit","ms"},{"ticksUnit","ns"},{"bucketSpec","ns-1us-1pct-60s-v1"},{"boundsNs",b},{"counts",c},{"overflow",std::to_string(overflow)},{"count",std::to_string(count)},{"sumNs",sum.convert_to<std::string>()},{"maxNs",count?json(std::to_string(max)):json(nullptr)},{"percentileMethod","nearest-rank-bucket-upper-bound"}}; }
};
struct ranges_t {
    std::map<int,std::map<std::uint64_t,std::uint64_t>> values;std::map<int,std::uint64_t> duplicates;
    bool add(int stream,std::uint64_t sequence) { auto &ranges=values[stream];auto next=ranges.upper_bound(sequence);auto previous=next;if(previous!=ranges.begin()){--previous;if(previous->second>=sequence){++duplicates[stream];return false;}}else previous=ranges.end();
        bool joinPrevious=previous!=ranges.end()&&previous->second!=UINT64_MAX&&previous->second+1==sequence;bool joinNext=sequence!=UINT64_MAX&&next!=ranges.end()&&next->first==sequence+1;
        if(joinPrevious){previous->second=joinNext?next->second:sequence;if(joinNext)ranges.erase(next);}else if(joinNext){auto last=next->second;ranges.erase(next);ranges.emplace(sequence,last);}else ranges.emplace(sequence,sequence);return true; }
    json stream(int id) const {json result=json::array();auto found=values.find(id);if(found!=values.end())for(auto[first,last]:found->second)result.push_back({{"first",std::to_string(first)},{"last",std::to_string(last)}});return result;}
    std::uint64_t count() const {std::uint64_t total=0;for(const auto &[id,ranges]:values)for(auto[first,last]:ranges)total+=last-first+1;return total;}
};
}

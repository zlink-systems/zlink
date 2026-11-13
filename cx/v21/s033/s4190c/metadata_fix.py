from pathlib import Path
p=Path('framework/bench/grpc/cpp/client/bench_cpp_client.cpp');s=p.read_text();s=s.replace('''    void reset ()
    {
        completed.store''','''    nlohmann::json error_summary_json () const
    {
        auto value = nlohmann::json::array ();
        for (const auto &error : error_summary)
            value.push_back ({{"type", error.type}, {"message", error.message}, {"count", error.count}});
        return value;
    }

    void reset ()
    {
        completed.store''')
a=s.index('    std::function<void ()> boundary;',s.index('class driver_t'))
s=s[:a]+'''    json streams_metadata () const
    {
        return {{"count", stream_count ()},
                {"inFlightPerStream", _window <= 0 ? json (nullptr) : json (_window)},
                {"implementation", "one dedicated submit thread per stream; native completion resources"}};
    }
'''+s[a:]
s=s.replace('''                              {"failure", _failure}}
                           .dump ()};''','''                              {"failure", _failure},
                              {"submitConcurrency", _options.submit_concurrency},
                              {"streams", _ready ? _driver->streams_metadata () : json (nullptr)},
                              {"client_error_summary", _phase == "idle" || _phase == "failed"
                                  ? _counters.error_summary_json () : json (nullptr)},
                              {"client_error_other_count", _phase == "idle" || _phase == "failed"
                                  ? json (_counters.other_errors) : json (nullptr)}}
                           .dump ()};''')
a=s.index('        value["streams"] = {');b=s.index('        value["completed_at_close"]',a);s=s[:a]+'        value["streams"] = _driver->streams_metadata ();\n'+s[b:]
a=s.index('        value["client_error_summary"] = json::array ();');b=s.index('        value["client_error_other_count"]',a);s=s[:a]+'        value["client_error_summary"] = _counters.error_summary_json ();\n'+s[b:]
p.write_text(s)
p=Path('framework/bench/grpc/cpp/run_local.sh');s=p.read_text().replace('python3 - "$1" "${TARGET_RATE:-0}"', 'python3 - "$1" "$2" "${TARGET_RATE:-0}"');s=s.replace('import json, os, sys\npath, rate, source_cpus, target_cpus = sys.argv[1:]', '''import json, os, sys, urllib.request
path, source_url, rate, source_cpus, target_cpus = sys.argv[1:]''')
s=s.replace('metadata = result.setdefault("metadata", {})','''with urllib.request.urlopen(source_url + "/bench/stats", timeout=5) as response:
    source = json.load(response)
metadata = result.setdefault("metadata", {})
metadata["submitConcurrency"] = source["submitConcurrency"]
cell = result["cells"][0]
cell.setdefault("streams", source["streams"])
if cell.get("contaminated"):
    cell["errors"] = source["errors"]
    cell["client_error_summary"] = source["client_error_summary"]
    cell["client_error_other_count"] = source["client_error_other_count"]''')
s=s.replace('record_cell_controls "${cell_dir}/results.json"','record_cell_controls "${cell_dir}/results.json" "${source_stats_url}"');p.write_text(s)
p=Path('framework/bench/grpc/c/common/bench_common.hpp');s=p.read_text().replace('    result.warmup_abandoned = warmup.abandoned;', '    result.warmup_abandoned = warmup.abandoned;\n    result.stream_count = warmup.stream_count;');p.write_text(s)

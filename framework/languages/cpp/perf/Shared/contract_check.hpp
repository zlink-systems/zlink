#pragma once
#include "measurement.hpp"

namespace perf {
inline json contract_check(){const auto fixture=read_json(std::string(ZLINK_PERF_CONTRACT_DIR)+"/fixtures/aggregation.json");for(int size:{64,4096}){const auto value=read_json(std::string(ZLINK_PERF_CONTRACT_DIR)+"/fixtures/payload-"+std::to_string(size)+".json");if(value.at("payload")!=pattern(size)||value.at("logicalBytes")!=size)throw std::runtime_error("Shared payload fixture differs.");validate_payload(value,pattern(size));auto wrong=value;auto bytes=wrong.at("payload").get<std::string>();bytes[bytes.size()/2]=bytes[bytes.size()/2]=='A'?'B':'A';wrong["payload"]=bytes;bool rejected=false;try{validate_payload(wrong,pattern(size));}catch(const validation_error &){rejected=true;}if(!rejected)throw std::runtime_error("Corrupted full payload was accepted.");}
    histogram_t histogram;for(const auto &sample:fixture.at("histogramSamplesNs"))histogram.record(std::stoll(sample.get<std::string>()));if(histogram.overflow!=decimal(fixture.at("expectedOverflow"))||histogram.bounds.size()!=1797)throw std::runtime_error("Shared histogram boundary/overflow fixture differs.");
    ranges_t ranges;for(const auto &range:fixture.at("ranges").at("source"))for(auto n=decimal(range.at("first"));n<=decimal(range.at("last"));++n)ranges.add(0,n);if(ranges.stream(0)!=fixture.at("ranges").at("source")||ranges.add(0,3)||ranges.duplicates[0]!=1)throw std::runtime_error("Range compaction/deduplication fixture differs.");
    json workload=read_json(std::string(ZLINK_PERF_CONTRACT_DIR)+"/matrix.json").at("defaults");workload["logicalStreams"]=1;json config={{"runId","contract-check"},{"cellId","contract-check"},{"configHash","contract-check"},{"role","source"},{"roleInstance",0},{"scenario","channel-echo-only"},{"source",true},{"mode","request"},{"terminal","ordinary"},{"workload",workload},{"provenance",{{"comparisonKey","contract-check"}}}};for (const char *key : {"connections", "logicalStreams"}) {
        auto bounded = config;
        bounded["workload"][key] = 1000;
        (void) config_t{bounded};
        bounded["workload"][key] = 1001;
        bool rejected = false;
        try { (void) config_t{bounded}; }
        catch (const std::invalid_argument &) { rejected = true; }
        if (!rejected) throw std::runtime_error("CCU above 1000 was accepted.");
    }
    measurement_t m(config_t{config},true);m.reset_seq="1";m.start=ticks();m.end=m.start+5000000000LL;m.start_unix=unix_ms();m.end_unix=unix_ms();m.settled=m.end;m.phase="complete";m.sealed=true;m.hist.emplace("latencyMs",std::move(histogram));const auto request=m.request(0,1);const auto reply=m.reply(request);fw::serializer_registry_t serializers;const auto serializer=serializers.get<reply_t>();const auto decoded=serializer.deserialize(serializer.serialize(reply));m.validate_reply(request,decoded);const auto cpu_fixture=read_json(std::string(ZLINK_PERF_CONTRACT_DIR)+"/fixtures/cpu-samples.json");m.cpu_samples=cpu_fixture.at("samples");for(const auto &sample:m.cpu_samples){const auto bin=sample.at("binIndex").get<std::size_t>();auto &totals=m.cpu_bins[bin];totals.first+=decimal(sample.at("observedDurationNs"));totals.second+=decimal(sample.at("cpuDeltaNs"));}const auto series=m.time_series();for(std::size_t i=0;i<cpu_fixture.at("expectedCpuPercent").size();++i)if(series.at(i).at("cpuPercent")!=cpu_fixture.at("expectedCpuPercent").at(i))throw std::runtime_error("Shared CPU actual-span weighted fixture differs.");auto snapshot=m.snapshot();json snapshots=json::array(),messages=json::array();
    const auto matrix=read_json(std::string(ZLINK_PERF_CONTRACT_DIR)+"/matrix.json");for(const auto &cell:matrix.at("cells")){
        auto mode_config=config;mode_config["scenario"]=cell.at("scenario");mode_config["mode"]=cell.at("mode");mode_config["terminal"]=cell.at("terminal");mode_config["workload"]["logicalStreams"]=cell.at("logicalStreams");mode_config["workload"]["connections"]=cell.at("connections");
        for(const bool owner:{true,false}){if(!owner&&cell.at("mode")!="publish"&&cell.at("mode")!="send")continue;
            measurement_t vector(config_t{mode_config},owner);vector.reset_seq="1";vector.start=m.start;vector.end=m.end;vector.start_unix=m.start_unix;vector.end_unix=m.end_unix;vector.settled=vector.end;vector.phase="complete";vector.sealed=true;vector.cpu_samples=m.cpu_samples;vector.cpu_bins=m.cpu_bins;vector.prepare_histograms();
            const auto echo=vector.request(0,1);const auto response=vector.reply(echo);vector.validate_reply(echo,response);messages.push_back({{"definition","PerfEchoRequest"},{"value",echo.fields}});messages.push_back({{"definition","PerfEchoReply"},{"value",response.fields}});
            if(vector.config.publish()){auto event=echo.fields;for(const auto *key:{"clientId","correlationId","returnSpotId","returnChannel"})event.erase(key);event["topic"]="perf.echo";const auto event_serializer=serializers.get<event_t>();auto typed=event_serializer.deserialize(event_serializer.serialize(event_t{event}));validate_payload(typed.fields,pattern(4096));messages.push_back({{"definition","PerfPublishEvent"},{"value",typed.fields}});}
            snapshots.push_back(vector.snapshot());
        }
    }return {{"ok",true},{"snapshots",snapshots},{"messages",messages},{"histogramBoundaryVectors",fixture.at("histogramSamplesNs").size()},{"payloadVectors",2},{"snapshot",snapshot}};
}
}

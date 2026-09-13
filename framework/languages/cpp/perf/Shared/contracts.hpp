#pragma once
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace perf {
using json = nlohmann::json;
inline json read_json(const std::string &file) { std::ifstream stream(file); if (!stream) throw std::runtime_error("Cannot open " + file); json value; stream >> value; return value; }
inline std::int64_t ticks() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
inline std::string unix_ms() { return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()); }
struct public_failure_t : std::runtime_error {std::string error_namespace,public_name,terminal;int code;public_failure_t(std::string space,std::string name,int value,std::string outcome,std::string message):std::runtime_error(std::move(message)),error_namespace(std::move(space)),public_name(std::move(name)),terminal(std::move(outcome)),code(value){}};
struct validation_error : std::runtime_error { std::string kind; validation_error(std::string value, std::string message) : std::runtime_error(std::move(message)), kind(std::move(value)) {} };
inline std::uint64_t decimal(const json &value) {
    if (!value.is_string()) throw validation_error("SchemaMismatch", "Decimal string required.");
    const auto text=value.get<std::string>(); std::size_t consumed=0;
    if(text.empty() || (text.size()>1 && text.front()=='0') || text.front()=='-') throw validation_error("SchemaMismatch", "Noncanonical unsigned decimal.");
    const auto result=std::stoull(text,&consumed); if(consumed!=text.size() || std::to_string(result)!=text) throw validation_error("SchemaMismatch", "Noncanonical decimal."); return result;
}
inline std::int64_t signed_decimal(const json &value) {
    if(!value.is_string())throw validation_error("SchemaMismatch","Signed decimal string required.");
    const auto &text=value.get_ref<const std::string&>();std::size_t consumed=0;const auto result=std::stoll(text,&consumed);
    if(consumed!=text.size()||std::to_string(result)!=text)throw validation_error("SchemaMismatch","Noncanonical signed decimal.");return result;
}
inline std::string pattern(std::size_t size) {
    static constexpr char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<unsigned char> bytes(size); for(std::size_t i=0;i<size;++i) bytes[i]=(31*i+17*(i/251)+29)%256;
    std::string out; out.reserve((size+2)/3*4);
    for(std::size_t i=0;i<size;i+=3) { const unsigned a=bytes[i],b=i+1<size?bytes[i+1]:0,c=i+2<size?bytes[i+2]:0; out+=alphabet[a>>2];out+=alphabet[((a&3)<<4)|(b>>4)];out+=i+1<size?alphabet[((b&15)<<2)|(c>>6)]:'=';out+=i+2<size?alphabet[c&63]:'='; } return out;
}
// The default Framework JSON serializer invokes these typed DTO conversions.
// No registration, alternate serializer or manual frame codec is used.
struct request_t { static constexpr const char *packet_name="PerfEchoRequest"; json fields; };
struct reply_t { static constexpr const char *packet_name="PerfEchoReply"; json fields; };
struct drive_t { static constexpr const char *packet_name="PerfDriveRequest"; json fields; };
struct drive_reply_t { static constexpr const char *packet_name="PerfDriveReply"; json fields; };
struct event_t { static constexpr const char *packet_name="PerfPublishEvent"; json fields; };
struct bind_t { static constexpr const char *packet_name="PerfBindRequest"; json fields; };
struct bind_reply_t { static constexpr const char *packet_name="PerfBindReply"; json fields; };
struct probe_t { static constexpr const char *packet_name="PerfProbeRequest"; json fields; };
#define PERF_JSON(Type) inline void to_json(json &out,const Type &value){out=value.fields;} inline void from_json(const json &in,Type &value){value.fields=in;}
PERF_JSON(request_t) PERF_JSON(reply_t) PERF_JSON(drive_t) PERF_JSON(drive_reply_t) PERF_JSON(event_t) PERF_JSON(bind_t) PERF_JSON(bind_reply_t) PERF_JSON(probe_t)
#undef PERF_JSON
inline void validate_payload(const json &data,const std::string &expected) {
    if(!data.at("payload").is_string() || data.at("payload").get<std::string>()!=expected) throw validation_error("PayloadMismatch", "Canonical logical byte pattern differs.");
    // Base64 is an application DTO field, not a Framework frame codec. Decode
    // every logical byte and run the same validation loop in every language.
    const auto &text=data.at("payload").get_ref<const std::string&>();
    const auto size=text.size()/4*3-(text.ends_with("==")?2:text.ends_with("=")?1:0);
    auto sextet=[](char ch)->unsigned { if(ch>='A'&&ch<='Z')return ch-'A';if(ch>='a'&&ch<='z')return ch-'a'+26;if(ch>='0'&&ch<='9')return ch-'0'+52;if(ch=='+')return 62;if(ch=='/')return 63;return 0; };
    std::vector<unsigned char> bytes;bytes.reserve(size);
    for(std::size_t i=0;i<text.size();i+=4){const auto word=(sextet(text[i])<<18)|(sextet(text[i+1])<<12)|(sextet(text[i+2])<<6)|sextet(text[i+3]);for(int shift:{16,8,0})if(bytes.size()<size)bytes.push_back((word>>shift)&255);}
    for(std::size_t i=0;i<bytes.size();++i)if(bytes[i]!=(31*i+17*(i/251)+29)%256)throw validation_error("PayloadMismatch","Logical payload pattern differs.");
}
inline void validate_identity(const json &request,const json &reply) {
    for(const auto *key:{"runId","cellId","resetSeq","phase","clientId","sequence","correlationId"}) if(request.at(key)!=reply.at(key)) throw validation_error("IdentityMismatch",std::string("Reply differs: ")+key);
    signed_decimal(reply.at("receivedTicks"));
    if(reply.at("clockDomainId").get<std::string>().empty()) throw validation_error("IdentityMismatch","Missing reply clock domain.");
}
inline json reason(std::string code,std::string text) { return {{"code",code},{"reason",text},{"owner","perf/README.ko.md"}}; }
struct config_t {
    json value;
    const json &workload() const { return value.at("workload"); }
    bool diagnostics_enabled() const {const auto d=value.value("diagnostics",json(nullptr));if(d.is_string())return d.get<std::string>()=="Normal";if(d.is_object())return d.value("level","")=="Normal";return value.at("provenance").value("diagnostics","")=="Normal";}
    bool source() const { return value.value("source",false); }
    std::string scenario() const { return value.at("scenario"); }
    bool cs() const { return scenario().starts_with("cs-") || scenario()=="session-echo-only"; }
    bool actor() const { return scenario().find("actor")!=std::string::npos && scenario()!="session-echo-only"; }
    bool spot() const { return scenario().find("spot")!=std::string::npos; }
    bool driven() const { return scenario().starts_with("s2s-spot-to-channel"); }
    bool publish() const { return value.value("mode","request")=="publish"; }
    bool send() const { return value.value("mode","request")=="send"; }
    bool correlated() const { return value.value("mode","request")=="send-send"; }
    bool worker() const { return scenario()=="spot-worker-offload-echo"; }
    bool yielding() const { return value.value("terminal","ordinary")=="yield"; }
    int request_bytes() const { return workload().at(send()||correlated()||publish()?"sendPayloadBytes":"requestPayloadBytes"); }
    int timeout() const { return workload().at("requestTimeoutMs"); }
    std::string text(const char *key,std::string fallback={}) const { return value.contains(key)&&value[key].is_string()?value[key].get<std::string>():fallback; }
    std::string target(const char *key,int stream) const { const auto &ids=value.at(key);if(ids.empty()) throw validation_error("IdentityMismatch", "Missing target IDs.");return ids.at(stream%ids.size()); }
};
}

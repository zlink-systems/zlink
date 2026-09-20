/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#include <service_wire_codec.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>
namespace generated = zlink::framework::runtime::protocol::generated;
namespace {
std::vector<std::uint8_t> from_hex(std::string_view hex) {
    const auto digit=[](char c)->std::uint8_t{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;throw std::invalid_argument("hex");};
    if(hex.size()%2)throw std::invalid_argument("hex length");
    std::vector<std::uint8_t> out;out.reserve(hex.size()/2);
    for(std::size_t i=0;i<hex.size();i+=2)out.push_back(static_cast<std::uint8_t>((digit(hex[i])<<4)|digit(hex[i+1])));
    return out;
}
nlohmann::json load_json(const std::filesystem::path&p){std::ifstream in(p);if(!in.good())throw std::runtime_error("fixture open: "+p.string());return nlohmann::json::parse(in);}
const nlohmann::json& pointer(const nlohmann::json&fixture,const nlohmann::json&pointers,const char*name){return fixture.at(nlohmann::json::json_pointer(pointers.at(name).get<std::string>()));}
template<class Result,class Encode> bool exact(Result decoded,Encode encode,const std::vector<std::uint8_t>&bytes,generated::error_code&error){error=decoded.error;if(!decoded)return false;const auto encoded=encode(decoded.value);error=encoded.error;return encoded&&encoded.value==bytes;}
bool command(std::uint8_t id,const std::vector<std::vector<std::uint8_t>>&frames,generated::error_code&error){
    if(id==28){const auto d=generated::decode_actorJoin_28_frames(frames);error=d.error;if(!d)return false;const auto e=generated::encode_actorJoin_28_frames(d.value);error=e.error;return e&&e.value==frames;}
    if(frames.size()!=1){error=generated::error_code::trailing;return false;}const auto&b=frames.front();
    switch(id){case 47:return exact(generated::decode_userSpotCreate_47(b),generated::encode_userSpotCreate_47,b,error);case 48:return exact(generated::decode_userSpotClose_48(b),generated::encode_userSpotClose_48,b,error);case 49:return exact(generated::decode_actorCreate_49(b),generated::encode_actorCreate_49,b,error);default:error=generated::error_code::header;return false;}
}
bool type(std::string_view name,const std::vector<std::uint8_t>&b,generated::error_code&error){
    if(name=="authority-payload-v1")return exact(generated::decode_durable_authority_payload_v1(b),generated::encode_durable_authority_payload_v1,b,error);
    if(name=="instance-activation-recovery-v1")return exact(generated::decode_durable_instance_activation_recovery_v1(b),generated::encode_durable_instance_activation_recovery_v1,b,error);
    if(name=="relocation-data-chunk-v1")return exact(generated::decode_durable_relocation_data_chunk_v1(b),generated::encode_durable_relocation_data_chunk_v1,b,error);
    if(name=="relocation-manifest-v1")return exact(generated::decode_durable_relocation_manifest_v1(b),generated::encode_durable_relocation_manifest_v1,b,error);
    if(name=="relocation-envelope-v1")return exact(generated::decode_relocation_envelope_v1(b),generated::encode_relocation_envelope_v1,b,error);
    error=generated::error_code::header;return false;
}
std::vector<std::vector<std::uint8_t>> bytes(const nlohmann::json&fixture,const nlohmann::json&item){const auto&p=item.at("pointers");if(p.contains("framesHex")){std::vector<std::vector<std::uint8_t>> out;for(const auto&hex:pointer(fixture,p,"framesHex"))out.push_back(from_hex(hex.get<std::string>()));return out;}for(const char*name:{"encodedHex","logicalHex","hex"})if(p.contains(name))return {from_hex(pointer(fixture,p,name).get<std::string>())};throw std::runtime_error("encoded pointer missing");}
}
int main(){
    const std::filesystem::path index_path=ZLINK_SERVICE_WIRE_FIXTURE_INDEX_PATH;const auto index=load_json(index_path);const auto protocol_root=index_path.parent_path().parent_path().parent_path();bool passed=true;
    for(const auto&entry:index.at("fixtures")){const auto fixture=load_json(protocol_root/entry.at("goldenFixture").get<std::string>());const auto kind=entry.at("kind").get<std::string>();const auto surface=entry.at("surface");
        const auto verify=[&](const nlohmann::json&item,bool valid){const auto frames=bytes(fixture,item);generated::error_code error=generated::error_code::ok;const bool accepted=kind=="command"?command(surface.at("commandId").get<std::uint8_t>(),frames,error):(frames.size()==1&&type(surface.at("type").get<std::string>(),frames.front(),error));if(accepted!=valid){std::cerr<<kind<<':'<<surface.at("format").get<std::string>()<<':'<<item.at("name").get<std::string>()<<": expected "<<(valid?"accept":"reject")<<", error-code="<<static_cast<int>(error)<<'\n';passed=false;}};
        for(const auto&item:entry.at("canonical"))verify(item,true);for(const auto&item:entry.at("malformed"))verify(item,false);
    }return passed?0:1;
}

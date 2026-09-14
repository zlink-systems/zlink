#include "Server/application.hpp"
#include "Client/application.hpp"
#include "Shared/contract_check.hpp"
#include <iostream>

int main(int argc,char **argv){try{if(argc==2&&std::string(argv[1])=="--contract-check"){std::cout<<perf::contract_check().dump()<<std::endl;return 0;}if(argc==3&&std::string(argv[1])=="--config")return perf::run_server(perf::config_t{perf::read_json(argv[2])},argc,argv);if(argc==5&&std::string(argv[1])=="--endpoint-config"&&std::string(argv[3])=="--client-index")return perf::run_client(argv[2],std::stoi(argv[4]));throw std::invalid_argument("Use --config <role.json> or --endpoint-config <endpoints.json> --client-index N.");}catch(const std::exception &error){std::cerr<<perf::json{{"ok",false},{"errorType",typeid(error).name()},{"message",error.what()}}.dump()<<std::endl;return 1;}}

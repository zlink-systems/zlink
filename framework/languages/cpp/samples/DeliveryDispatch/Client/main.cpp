/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "delivery_dispatch_client_scenario.hpp"

using namespace zlink;

#include <iostream>
#include <optional>
#include <string>

namespace
{

/* Standalone client는 직접 연결하는 endpoint만 명시적인 CLI option으로 받는다. 값은 시작할 때
 * 한 번 검증한다(공통 정책 sample-e2e-configuration-policy.ko.md §4). */
struct client_options_t
{
    std::string api_url;
    std::string stream_endpoint;
    std::string courier_stream_endpoint;
};

std::string read_option (int argc, char **argv, const std::string &name)
{
    for (int index = 1; index + 1 < argc; ++index) {
        if (argv[index] == name) {
            return argv[index + 1];
        }
    }
    return {};
}

std::optional<client_options_t> read_client_options (int argc, char **argv)
{
    client_options_t options;
    options.api_url = read_option (argc, argv, "--api-url");
    options.stream_endpoint = read_option (argc, argv, "--stream-endpoint");
    options.courier_stream_endpoint = read_option (argc, argv, "--courier-stream-endpoint");
    if (options.api_url.empty () || options.stream_endpoint.empty ()
        || options.courier_stream_endpoint.empty ()) {
        return std::nullopt;
    }
    return options;
}

} // namespace

int main (int argc, char **argv)
{
    const auto options = read_client_options (argc, argv);
    if (!options) {
        std::cerr << "usage: " << argv[0]
                  << " --api-url <url> --stream-endpoint <endpoint>"
                     " --courier-stream-endpoint <endpoint>\n";
        return 2;
    }
    if (!zlink::samples::deliverydispatch::delivery_dispatch_client_scenario_t{}.run (
          options->api_url, options->stream_endpoint, options->courier_stream_endpoint)) {
        std::cerr << "deliverydispatch=failed\n";
        return 1;
    }
    std::cout << "deliverydispatch=completed\n";
    return 0;
}

/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "shoppingmall_client_scenario.hpp"

#include <exception>
#include <iostream>
#include <string>

namespace
{

std::string read_option (int argc, char **argv, const std::string &name)
{
    for (int index = 1; index + 1 < argc; ++index) {
        if (argv[index] == name) {
            return argv[index + 1];
        }
    }
    return {};
}

} // namespace

int main (int argc, char **argv)
{
    const auto api_a_http_url = read_option (argc, argv, "--api-a-http-url");
    const auto api_b_http_url = read_option (argc, argv, "--api-b-http-url");
    const auto resume_order_id = read_option (argc, argv, "--resume-order-id");
    const auto projection_continue_order_id =
      read_option (argc, argv, "--projection-continue-order-id");
    const auto projection_rebuild_order_id =
      read_option (argc, argv, "--projection-rebuild-order-id");
    if (api_a_http_url.empty () || api_b_http_url.empty () || resume_order_id.empty ()
        || projection_continue_order_id.empty () || projection_rebuild_order_id.empty ()) {
        std::cerr << "usage: " << argv[0]
                  << " --api-a-http-url <url> --api-b-http-url <url> --resume-order-id <id>"
                     " --projection-continue-order-id <id> --projection-rebuild-order-id <id>\n";
        return 2;
    }
    try {
        zlink::samples::shoppingmall::shoppingmall_client_scenario_t{}.run (api_a_http_url,
                                                                            api_b_http_url,
                                                                            resume_order_id,
                                                                            projection_continue_order_id,
                                                                            projection_rebuild_order_id);
        std::cout << "shoppingmall=completed" << std::endl;
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << "shoppingmall client failed: " << error.what () << std::endl;
        return 1;
    }
}

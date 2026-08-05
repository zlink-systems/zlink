/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Service/Support/service_host.hpp"

namespace zlink::framework::e2e::runtime_monitoring::throwing_service
{

inline int run_throwing_service_host (int argc, char **argv)
{
    return service::run_service_host (argc, argv);
}

} // namespace zlink::framework::e2e::runtime_monitoring::throwing_service

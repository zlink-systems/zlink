/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "service_host_factory.hpp"

int main (int argc, char **argv)
{
    return zlink::framework::e2e::runtime_monitoring::service::run_all_service_host (argc,
                                                                                     argv);
}

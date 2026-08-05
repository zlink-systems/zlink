/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "filtered_service_host_factory.hpp"

int main (int argc, char **argv)
{
    return zlink::framework::e2e::runtime_monitoring::filtered_service::
      run_filtered_service_host (argc, argv);
}

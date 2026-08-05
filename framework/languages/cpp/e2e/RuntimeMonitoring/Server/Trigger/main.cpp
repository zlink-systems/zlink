/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "trigger_host_factory.hpp"

int main (int argc, char **argv)
{
    return zlink::framework::e2e::runtime_monitoring::trigger::run_trigger_host (argc, argv);
}

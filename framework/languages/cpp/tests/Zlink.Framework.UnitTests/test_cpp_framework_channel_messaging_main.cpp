/* SPDX-License-Identifier: Elastic-2.0 */
#include "../support/framework_channel_messaging_cases.hpp"

#include <zlink/framework/contracts/dispatch/task.hpp>

int main ()
{
    zlink::framework::completion_owner_t completion_owner;
    if (const int failure = framework_channel_messaging_tests::verify_flow_capture (); failure != 0)
        return failure;
    // Each stage calls the next before its local fixtures are destroyed.
    return framework_channel_messaging_tests::verify_channel_configuration ();
}

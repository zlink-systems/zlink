/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/stream_connector.hpp>
#include <zlink/stream_e2e_client.hpp>

int main ()
{
    zlink::stream_connector::connector_options_t options;
    options.endpoint = "tcp://127.0.0.1:1";
    auto connector = zlink::stream_connector::connector_factory_t::create (options);
    auto client = zlink::stream_e2e_client::use (connector);
    (void) client;
    return 0;
}

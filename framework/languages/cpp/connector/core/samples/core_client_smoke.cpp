/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/stream_connector.hpp>

int main ()
{
    zlink::stream_connector::connector_options_t options;
    options.endpoint = "tcp://127.0.0.1:1";
    auto connector = zlink::stream_connector::connector_factory_t::create (options);
    auto result = connector.connect ();
    connector.close ();
    return result ? 0 : 0;
}

// SPDX-License-Identifier: MPL-2.0
'use strict';
const zlink = require('@zlink-systems/zlink');
const { parseMultiArgs } = require('./perf_multi_common');
const { resolveRoutedPattern, runRoutedSendSendClient } = require('./perf_multi_routed_sendsend');
const { runSocketReqRepClient } = require('./perf_multi_socket_reqrep');
const SERVER_ROUTING_ID = zlink.RoutingId.from(Buffer.from('SERVER', 'ascii'));
async function main() {
  const options = parseMultiArgs(process.argv.slice(2));
  const pattern = resolveRoutedPattern(process.env.PERF_MULTI_PATTERN, 'ROUTER_ROUTER');
  if (pattern.endsWith('_SENDSEND')) {
    await runRoutedSendSendClient({ options, pattern, routerClient: true });
    return;
  }
  await runSocketReqRepClient({ options, pattern: 'MULTI_ROUTER_ROUTER_REQREP',
    routerClient: true, serverRoutingId: SERVER_ROUTING_ID });
}
main().catch((error) => { console.error(error); process.exitCode = 1; });

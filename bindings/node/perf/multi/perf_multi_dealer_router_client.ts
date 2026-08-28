// SPDX-License-Identifier: MPL-2.0
'use strict';
const { parseMultiArgs } = require('./perf_multi_common');
const { resolveRoutedPattern, runRoutedSendSendClient } = require('./perf_multi_routed_sendsend');
const { runSocketReqRepClient } = require('./perf_multi_socket_reqrep');
async function main() {
  const options = parseMultiArgs(process.argv.slice(2));
  const pattern = resolveRoutedPattern(process.env.PERF_MULTI_PATTERN, 'DEALER_ROUTER');
  if (pattern.endsWith('_SENDSEND')) {
    await runRoutedSendSendClient({ options, pattern, routerClient: false });
    return;
  }
  await runSocketReqRepClient({ options, pattern: 'MULTI_DEALER_ROUTER_REQREP',
    routerClient: false, serverRoutingId: null });
}
main().catch((error) => { console.error(error); process.exitCode = 1; });

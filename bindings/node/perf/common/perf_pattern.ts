// SPDX-License-Identifier: MPL-2.0

'use strict';

const MULTI_ECHO_PATTERNS = new Set([
  'DEALER_ROUTER',
  'DEALER_ROUTER_SENDSEND',
  'DEALER_ROUTER_REQREP',
  'ROUTER_ROUTER',
  'ROUTER_ROUTER_SENDSEND',
  'ROUTER_ROUTER_REQREP',
  'STREAM'
]);

const SINGLE_ECHO_PATTERNS = new Set([
  'DEALER_ROUTER_REQREP',
  'ROUTER_ROUTER_REQREP'
]);

function isEchoPattern(pattern, suite = 'single') {
  return suite === 'multi'
    ? MULTI_ECHO_PATTERNS.has(pattern)
    : SINGLE_ECHO_PATTERNS.has(pattern);
}

module.exports = { isEchoPattern };

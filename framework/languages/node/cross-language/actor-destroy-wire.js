/* SPDX-License-Identifier: Elastic-2.0 */

const binding = require('@zlink-systems/zlink');
const {
  ServiceWireCommand
} = require('../packages/framework/dist/runtime/foundation/service-wire-constants.generated');

// The TCP proxy observes frames on the wire. This observer counts attempts
// at the public binding boundary, including attempts that Core rejects.
function observeDestroySubmissions(emit) {
  const originalFactory = binding.createRouterSocket;
  let observed = false;
  binding.createRouterSocket = function (...args) {
    const socket = originalFactory.apply(this, args);
    for (const method of ['send', 'request']) {
      const original = socket[method];
      socket[method] = function (...args) {
        const operation = original.apply(this, args);
        const message = operation.message;
        const submit = operation.submit;
        let header;
        operation.message = function (part) {
          header ??= part;
          return message.call(this, part);
        };
        operation.submit = function () {
          if (
            header instanceof Uint8Array &&
            header[0] === 90 &&
            header[1] === 77 &&
            header[2] === 1 &&
            header[3] === ServiceWireCommand.actorDestroy
          ) {
            emit('command27Submitted');
          }
          return submit.call(this);
        };
        return operation;
      };
    }
    if (!observed) {
      observed = true;
      emit('submissionObserverReady');
    }
    return socket;
  };
  return () => {
    binding.createRouterSocket = originalFactory;
  };
}

module.exports = { observeDestroySubmissions };

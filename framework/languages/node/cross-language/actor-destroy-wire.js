/* SPDX-License-Identifier: Elastic-2.0 */

const assert = require('node:assert/strict');
const binding = require('@zlink-systems/zlink');
const {
  decodeActorDestroy27
} = require('../packages/framework/dist/runtime/protocol/service_wire_pilot_codec.generated');
const {
  decodeReplyHeader
} = require('../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');
const {
  ServiceWireCommand
} = require('../packages/framework/dist/runtime/foundation/service-wire-constants.generated');

function wireCommand(bytes) {
  return bytes instanceof Uint8Array && bytes[0] === 90 && bytes[1] === 77 && bytes[2] === 1
    ? bytes[3]
    : undefined;
}

// Observe the public binding operation where the Framework hands frames to Core.
function captureActorDestroyWire(peer, holdAdmission, emit) {
  const originalFactory = binding.createRouterSocket;
  let releaseAdmission;
  let admissionHeld = false;
  let finished = false;
  let observing = false;
  let malformed = false;
  const admission = new Promise((resolve) => {
    releaseAdmission = resolve;
  });
  const admitted = new Set();
  const oneWay = new Map();

  function decode(decoder, header) {
    try {
      return decoder(header);
    } catch (error) {
      malformed = true;
      emit('error', { message: String(error.message) });
      return undefined;
    }
  }

  binding.createRouterSocket = function (...args) {
    const socket = originalFactory.apply(this, args);
    observing = true;
    const recv = socket.recv;
    socket.recv = function (record, ...rest) {
      const received = recv.call(this, record, ...rest);
      if (received && oneWay.size > 0) {
        const header = record.parts[0]?.data();
        if (wireCommand(header) === ServiceWireCommand.reply) {
          const reply = decode(decodeReplyHeader, header);
          if (reply && oneWay.get(reply.correlation) === String(record.routingId)) {
            emit('command27Reply', { correlation: reply.correlation });
          }
        }
      }
      return received;
    };
    for (const method of ['send', 'request']) {
      const original = socket[method];
      socket[method] = function (target, ...rest) {
        const operation = original.call(this, target, ...rest);
        const message = operation.message;
        const submit = operation.submit;
        let header;
        let partCount = 0;
        operation.message = function (part) {
          if (partCount++ === 0) header = part;
          return message.call(this, part);
        };
        operation.submit = function () {
          const command = wireCommand(header);
          if (
            holdAdmission &&
            method === 'send' &&
            command === ServiceWireCommand.admit &&
            String(target) === peer
          ) {
            const route = socket.routesSnapshot().find((item) => String(item.routingId) === peer);
            if (!route) return submit.call(this);
            if (!admissionHeld) {
              admissionHeld = true;
              emit('admissionHeld', { peer, routeGeneration: route.routeGeneration });
            }
            return {
              result: binding.SubmitResult.Backpressured,
              admitted: admission.then(async (allowed) => {
                if (allowed) await submit.call(this).admitted;
              })
            };
          }
          if (command !== ServiceWireCommand.actorDestroy) return submit.call(this);
          const decoded = decode(decodeActorDestroy27, header);
          if (!decoded) return submit.call(this);
          const { correlation, actor } = decoded;
          emit('command27Submitted', { correlation });
          if (method === 'send') oneWay.set(correlation, String(target));
          const submission = submit.call(this);
          const sent = () =>
            emit('command27Sent', {
              correlation,
              flags: header[4],
              payloadFrames: partCount - 1,
              actorId: actor.id,
              generation: actor.generation,
              owner: String(binding.RoutingId.from(actor.targetNodeRid)),
              nodeGeneration: actor.targetNodeGeneration,
              ownerGeneration: actor.expectedAuthorityOwnerGeneration,
              leaseGeneration: actor.expectedOwnerLeaseGeneration
            });
          if (submission.result === binding.SubmitResult.Ok) sent();
          else {
            const observation = submission.admitted.then(sent, () => undefined);
            admitted.add(observation);
            void observation.finally(() => admitted.delete(observation));
          }
          if (method === 'request') {
            void submission.reply.then(
              () => emit('command27Reply', { correlation }),
              () => undefined
            );
          }
          return submission;
        };
        return operation;
      };
    }
    return socket;
  };

  return {
    stopHolding() {
      holdAdmission = false;
      releaseAdmission(false);
    },
    allow() {
      assert(admissionHeld && holdAdmission, 'no peer ADMIT is held');
      holdAdmission = false;
      emit('admissionAllowed');
      releaseAdmission(true);
    },
    async finish(complete = false) {
      if (finished) return;
      finished = true;
      await Promise.all(admitted);
      binding.createRouterSocket = originalFactory;
      if (complete && observing && !malformed) emit('wireObserved', { boundary: 'binding' });
    }
  };
}

module.exports = { captureActorDestroyWire };

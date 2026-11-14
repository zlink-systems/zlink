// SPDX-License-Identifier: MPL-2.0
'use strict';

// raw-service-mesh-runtime.ts: RAW_MESH_RECEIVE_RECORD_BUDGET / receiveBatch.
const RECEIVE_RECORD_BUDGET = 64;

function createReceivePermitPump(queue, receive, processMessage, onError, signal) {
  let rotation;
  let receivePermit;
  let receiveAcquisition;

  // Match acquireReceivePermit: immediate grants stay synchronous; a FIFO
  // capacity wait ends this round and saves its grant for the next round.
  function acquireReceivePermit() {
    let resolveAcquisition;
    let rejectAcquisition;
    const permit = queue.acquireOrResume(
      (permit) => {
        receiveAcquisition = undefined;
        receivePermit = permit;
        resolveAcquisition?.(permit);
      },
      (error) => rejectAcquisition?.(error),
      signal,
      'remote'
    );
    if (permit === undefined) {
      const acquisition = new Promise((resolve, reject) => {
        resolveAcquisition = resolve;
        rejectAcquisition = reject;
      });
      receiveAcquisition = acquisition;
      acquisition.catch((error) => {
        receiveAcquisition = undefined;
        if (!signal.aborted) onError(error, 'permit');
      });
    }
    return permit;
  }

  return function pump() {
    rotation ??= queue.createReceiveRotation(RECEIVE_RECORD_BUDGET, 'remote');
    rotation.reserve();
    let handled = 0;
    try {
      while (handled < RECEIVE_RECORD_BUDGET) {
        let permit = receivePermit;
        receivePermit = undefined;
        permit ??= rotation.take();
        if (permit === undefined && receiveAcquisition === undefined) {
          permit = acquireReceivePermit();
        }
        if (permit === undefined) break;
        try {
          let hasMessage;
          try {
            hasMessage = receive();
          } catch (error) {
            onError(error, 'recv');
            break;
          }
          if (!hasMessage) break;
          handled += 1;
          try {
            processMessage(permit);
          } catch (error) {
            onError(error, 'handler');
          }
        } finally {
          // Match receiveOne's no-data/error cleanup and finishReceive's
          // ingress owner close, including return of reusable permit storage.
          permit.releaseAfterInternalProcessing();
          permit.releaseStorage?.();
        }
      }
    } finally {
      rotation.releaseUnused();
    }
    return handled;
  };
}

module.exports = { createReceivePermitPump, RECEIVE_RECORD_BUDGET };

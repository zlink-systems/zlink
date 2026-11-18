const assert = require('node:assert/strict');
const test = require('node:test');
const {
  ZLinkHostSpotAddressTransport
} = require('../../packages/framework/dist/runtime/host/spot-address-transport');
const {
  createInternalFrameworkException,
  ZLinkFrameworkInternalErrorKind
} = require('../../packages/framework/dist/runtime/framework-errors-internal');

for (const ready of [false, true]) {
  for (const request of [false, true]) {
    for (const enabled of [false, true]) {
      test(`Instance target not Ready: ready route=${ready}, request=${request}, trace=${enabled}`, async () => {
        const events = [];
        const error = createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.RouteNotConnected,
          'target is not Ready'
        );
        if (!enabled) {
          Object.defineProperty(error, 'name', {
            get() {
              throw new Error('Off must not format the diagnostic error');
            }
          });
        }
        let submissions = 0;
        const reject = () => {
          submissions++;
          return Promise.reject(error);
        };
        const flow = {
          flowCreationEnabled: () => false,
          accepts: () => enabled,
          trace: (event) => events.push(event)
        };
        const target = {
          spotId: 'room',
          spotKind: 'instance',
          stableType: 'Room',
          routerChannelId: 'mesh',
          targetNodeRid: 'dead-target'
        };
        const node = {
          instanceSpotPlacementTypes: () => ['Room'],
          selectObjectPlacement: () => ({
            kind: 'selected',
            target: {
              targetNodeRid: 'dead-target',
              targetNodeGeneration: 7n,
              descriptorVersion: '1'
            }
          }),
          sendToMissingInstanceSpot: reject,
          requestToMissingInstanceSpot: reject
        };
        const transport = new ZLinkHostSpotAddressTransport({
          resolver: () => ({ resolve: async () => (ready ? target : undefined) }),
          routed: {
            sendToSpot: reject,
            requestToSpot: () => {
              submissions++;
              throw error;
            }
          },
          meshNames: () => ['mesh'],
          meshNode: () => node,
          completions: () => ({ submit: (submit) => Promise.resolve().then(submit) }),
          defaultRequestTimeoutMs: 1000,
          dispatchErrors: {
            flow,
            captureEnabled: () => enabled,
            report: (event) => events.push(event)
          }
        });
        class Ping {}
        const call = { instanceSpot: true, instanceSpotType: 'Room' };
        await assert.rejects(
          request
            ? transport.requestToSpotAddress('room', new Ping(), call)
            : transport.sendToSpotAddress('room', new Ping(), call),
          (failure) => failure === error
        );
        assert.equal(submissions, 1);
        const failures = events.filter(
          (event) => event.reason !== undefined || event.errorReason !== undefined
        );
        assert.equal(failures.length, enabled ? 1 : 0);
        if (enabled) {
          assert.equal(failures[0].targetRid, 'dead-target');
          assert.equal(failures[0].reason ?? failures[0].errorReason, 'stale_target');
          if (request) assert.equal(failures[0].error, error);
          else {
            assert.equal(failures[0].errorType, error.name);
            assert.equal(failures[0].errorMessage, error.message);
          }
        }
        assert.equal(events.length, enabled ? (request ? 2 : 1) : 0);
      });
    }
  }
}

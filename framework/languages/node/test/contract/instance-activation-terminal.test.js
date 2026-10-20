'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');
const {
  ZLinkLocationStoreRepository
} = require('../../packages/framework/dist/runtime/locations/location-store-repository');
const {
  ZLinkInMemoryProviderLocationStore
} = require('../../packages/framework/dist/runtime/locations/in-memory-provider-location-store');
const {
  ServiceStatefulRuntime
} = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');
const {
  encodeMultipartApplicationPayload
} = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');
const {
  encodeChannelEnvelopeParts
} = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const { runWithFlow } = require('../../packages/framework/dist/runtime/diagnostics/flow-context');
const {
  ZLinkBufferMessage
} = require('../../packages/framework/dist/runtime/backend/runtime-message');
const {
  SERVICE_FRAMEWORK_MULTIPART_PACKET_NAME,
  SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE
} = require('../../packages/framework/dist/runtime/foundation/service-wire-constants.generated');
const {
  ZLinkRuntimeTaskRunner,
  ZLinkRuntimeTaskErrorSink
} = require('../../packages/framework/dist/runtime/execution');
const {
  SERVICE_WIRE_COMMAND_OFFSET,
  SERVICE_WIRE_FLAGS_OFFSET
} = require('../../packages/framework/dist/runtime/foundation/service-wire-binary-primitives');

function providerFailure(signal) {
  const repository = new ZLinkLocationStoreRepository(new ZLinkInMemoryProviderLocationStore());
  // The provider checks this expired absolute deadline before reading authority.
  return repository.commit(
    { key: { kind: 'instance_spot', globalId: 'room' } },
    signal,
    BigInt(Date.now() - 1)
  );
}

for (const phase of ['commit', 'after-reserve', 'shutdown']) {
  test(`activation metric covers ${phase} after application preparation`, async (t) => {
    t.mock.timers.enable({ apis: ['Date'], now: 1800000000000 });
    const deadlineUnixMs = BigInt(Date.now() + 1);
    const outcomes = [];
    const metrics = {
      change() {},
      startInstanceSpotActivation: () => ({ complete: (outcome) => outcomes.push(outcome) })
    };
    class Room {}
    const manager = new framework.DefaultZLinkSpotManager({
      detachedTaskRunner: new ZLinkRuntimeTaskRunner(
        new ZLinkRuntimeTaskErrorSink(),
        new AbortController().signal
      ),
      spotFactories: [Room],
      instanceSpotFactories: new Map([['mesh', new Map([['Room', Room]])]]),
      metrics
    });
    const runtime = new ServiceStatefulRuntime(
      { setServiceIngress() {}, topology: {} },
      'target',
      7n
    );
    runtime.setDispatchErrorReporter({ metrics }, 'mesh');
    const repository = new ZLinkLocationStoreRepository(new ZLinkInMemoryProviderLocationStore());
    const record = {
      kind: 'instanceSpot',
      activation: 'missing',
      operationKind: 'send',
      deadlineUnixMs,
      operation: { high: 1n, low: 1n },
      sourceNodeRid: 'source',
      sourceNodeGeneration: 1n,
      target: {
        targetSpotId: 'room',
        stableType: 'Room',
        targetNodeRid: 'target',
        targetNodeGeneration: 7n,
        targetMeshName: 'mesh',
        descriptorVersion: '1'
      }
    };
    let prepared = false;
    runtime.registerAsyncInstanceActivationAuthority({
      read: async () => ({ kind: 'missing' }),
      async reserve() {
        if (phase === 'after-reserve') t.mock.timers.tick(2);
        if (phase === 'shutdown') runtime.close();
        return {
          kind: 'reserved',
          reservation: { attempt: 1n, authorityOwnerGeneration: 1n, token: 'reservation' }
        };
      },
      commit: () =>
        repository.commit(
          { key: { kind: 'instance_spot', globalId: 'room' } },
          undefined,
          deadlineUnixMs
        ),
      abort: async () => {}
    });
    runtime.registerInstanceApplicationLifecycle({
      isMaterialized: () => false,
      async materialize() {
        await manager.materializeInstance('mesh', 'Room', 'room', 1n);
        prepared = true;
        assert.deepEqual(outcomes, [], 'Preparation must not report Ready before commit.');
        t.mock.timers.tick(2);
      },
      discard: async () => {},
      beginTerminal() {},
      completeTerminal: async () => false
    });
    try {
      await assert.rejects(
        runtime.activateMissingInstanceAsync(record, Buffer.from('{}'), undefined, () =>
          assert.fail('Failure admitted.')
        ),
        {
          kind:
            phase === 'shutdown'
              ? framework.ZLinkFrameworkErrorKind.ShuttingDown
              : framework.ZLinkFrameworkErrorKind.DeadlineExceeded
        }
      );
      assert.equal(prepared, phase === 'commit');
      assert.deepEqual(outcomes, [phase === 'shutdown' ? 'shutdown' : 'timed_out']);
    } finally {
      runtime.close();
      await manager.discardInstance('mesh', 'room');
    }
  });
}

test('provider Ready commit preserves DeadlineExceeded instead of cancellation or provider failure', async () => {
  await assert.rejects(providerFailure(), (error) => {
    assert.ok(error instanceof framework.ZLinkFrameworkException);
    assert.equal(error.kind, framework.ZLinkFrameworkErrorKind.DeadlineExceeded);
    return true;
  });
});

test('actual cancellation takes precedence over an expired activation deadline', async () => {
  const cancellation = new AbortController();
  cancellation.abort();
  await assert.rejects(
    providerFailure(cancellation.signal),
    (error) => error === cancellation.signal.reason
  );
});

test('disabled activation diagnostics do not decode the retained message or inspect its error', () => {
  const runtime = new ServiceStatefulRuntime({ setServiceIngress() {} }, 'target', 7n);
  runtime.setDispatchErrorReporter(
    {
      captureEnabled: () => false,
      flow: { accepts: () => false, flowCreationEnabled: () => false }
    },
    'mesh'
  );
  try {
    runtime.finishMissingInstanceActivationFailure(
      {},
      { operationKind: 'send' },
      {
        get name() {
          assert.fail('Disabled diagnostics inspected the error.');
        }
      },
      undefined,
      Buffer.alloc(0)
    );
  } finally {
    runtime.close();
  }
});

for (const kind of ['deadline', 'rejected', 'shutdown']) {
  test(`activation metric consumes typed ${kind} result`, async () => {
    const error =
      kind === 'deadline'
        ? await providerFailure().catch((error) => error)
        : kind === 'shutdown'
          ? new DOMException('Runtime stopped.', 'AbortError')
          : new framework.ZLinkFrameworkException(
              framework.ZLinkFrameworkErrorKind.Rejected,
              'Factory rejected.'
            );
    const outcomes = [];
    const runtime = new ServiceStatefulRuntime({ setServiceIngress() {} }, 'target', 7n);
    runtime.setDispatchErrorReporter(
      {
        metrics: {
          startInstanceSpotActivation: () => ({ complete: (outcome) => outcomes.push(outcome) })
        }
      },
      'mesh'
    );
    runtime.runMissingInstanceActivation = async () => {
      throw error;
    };
    try {
      await assert.rejects(
        runtime.activateMissingInstanceAsync(
          {
            target: { targetSpotId: 'room', targetMeshName: 'mesh', stableType: 'Room' }
          },
          Buffer.alloc(0),
          undefined,
          () => assert.fail('Failed activation was admitted.')
        ),
        (actual) => actual === error
      );
      assert.deepEqual(outcomes, [
        kind === 'deadline' ? 'timed_out' : kind === 'shutdown' ? 'shutdown' : 'rejected'
      ]);
    } finally {
      runtime.close();
    }
  });
  for (const local of [false, true]) {
    test(`${local ? 'local' : 'remote'} one-way ${kind} has exactly one original-flow drop`, async () => {
      const error =
        kind === 'deadline'
          ? await providerFailure().catch((error) => error)
          : kind === 'shutdown'
            ? new DOMException('Runtime stopped.', 'AbortError')
            : new framework.ZLinkFrameworkException(
                framework.ZLinkFrameworkErrorKind.Rejected,
                'Factory rejected.'
              );
      const flow = { flowId: '019a0000-0000-7000-8000-000000000001', flowOrigin: 'Application' };
      const payload = runWithFlow(flow, () =>
        encodeMultipartApplicationPayload(
          encodeChannelEnvelopeParts(3, 'instance', 'Notice', {}).map((part) =>
            ZLinkBufferMessage.retainPayload(ZLinkBufferMessage.from(part))
          ),
          SERVICE_FRAMEWORK_MULTIPART_PACKET_NAME,
          SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE
        )
      );
      const events = [];
      const reports = [];
      let finished;
      const terminal = new Promise((resolve) => {
        finished = resolve;
      });
      const owner = {
        retain() {
          return this;
        },
        close() {}
      };
      const runtime = new ServiceStatefulRuntime(
        { setServiceIngress() {}, reserveLocalIngress: async () => owner },
        'target',
        7n
      );
      const source = local
        ? runtime
        : new ServiceStatefulRuntime(
            {
              setServiceIngress() {},
              async sendService(_target, parts) {
                await runtime.ingress({
                  sourceRoutingId: 'source',
                  sourceNodeGeneration: 1n,
                  parts,
                  command: parts[0][SERVICE_WIRE_COMMAND_OFFSET],
                  flags: parts[0][SERVICE_WIRE_FLAGS_OFFSET],
                  applicationJobOwner: owner
                });
                return true;
              }
            },
            'source',
            1n
          );
      runtime.setDispatchErrorReporter(
        {
          captureEnabled: () => true,
          report: (event) => {
            reports.push(event);
            finished();
          },
          flow: {
            accepts: () => true,
            flowCreationEnabled: () => true,
            trace: (event) => events.push(event)
          }
        },
        'mesh'
      );
      runtime.validateInstanceIngress = () => {};
      runtime.registerAsyncInstanceActivationAuthority({});
      runtime.runMissingInstanceActivation = async () => {
        throw error;
      };
      const record = {
        kind: 'instanceSpot',
        activation: 'missing',
        target: {
          targetSpotId: 'room',
          stableType: 'Room',
          targetNodeRid: 'target',
          targetNodeGeneration: 7n,
          targetMeshName: 'mesh',
          descriptorVersion: '1'
        },
        sourceNodeRid: local ? 'target' : 'source',
        sourceNodeGeneration: 1n,
        operation: { high: 1n, low: 1n },
        operationKind: 'send',
        deadlineUnixMs: BigInt(Date.now() - 1)
      };
      try {
        // Detached activation runs without the source ambient context.
        await source.sendToMissingInstanceSpotFrame(record.target, payload, record.deadlineUnixMs);
        await terminal;
        await new Promise((resolve) => setImmediate(resolve));
        assert.equal(events.length, 1);
        assert.equal(events[0].outcome, 'dropped');
        assert.equal(events[0].surface, 'instance_spot');
        assert.equal(
          events[0].errorReason,
          kind === 'deadline'
            ? 'activation_timeout'
            : kind === 'shutdown'
              ? 'shutdown'
              : 'activation_rejected'
        );
        assert.equal(events[0].flowId, flow.flowId);
        assert.equal(events[0].flowOrigin, flow.flowOrigin);
        assert.equal(events[0].errorType, error.name);
        assert.equal(events[0].errorMessage, error.message);
        assert.equal(reports.length, 1);
        assert.equal(reports[0].error, error);
      } finally {
        runtime.close();
        if (!local) source.close();
      }
    });
  }
}

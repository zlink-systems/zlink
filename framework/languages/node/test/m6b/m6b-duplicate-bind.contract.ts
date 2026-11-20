import assert from 'node:assert/strict';
import { test } from 'node:test';
import { readFileSync } from 'node:fs';
import { ServiceStatefulRuntime } from '../../packages/framework/src/runtime/foundation/service-stateful-runtime';
import type { RawServiceMeshRuntime } from '../../packages/framework/src/runtime/foundation/raw-service-mesh-runtime';
import { ApplicationIngressRecordOwner } from '../../packages/framework/src/runtime/application-jobs/application-ingress-record-owner';
import {
  ApplicationJobQueue,
  resolveApplicationJobQueueConfiguration
} from '../../packages/framework/src/runtime/host/application-job-queue';

import { RequestResult, SubmitResult } from '@zlink-systems/zlink';
import { ZLinkFrameworkRuntimeHost } from '../../packages/framework/src/runtime/host';
import {
  ReadyDomain,
  ReceiveKind,
  type ReceiveRecord
} from '../../packages/framework/src/runtime/foundation/service-runtime-contracts';

type BindingIdentity = {
  readonly sessionNodeRid: string;
  readonly sessionRid: string;
  readonly sessionOwnerNodeGeneration: bigint;
  readonly bindingGeneration: bigint;
};

function binding(sessionNodeRid: string, sessionRid: string, generation: bigint): BindingIdentity {
  return {
    sessionNodeRid,
    sessionRid,
    sessionOwnerNodeGeneration: generation + 10n,
    bindingGeneration: generation
  };
}

function makeFixture(
  callback?: (
    current: Record<string, unknown>,
    incoming: Record<string, unknown>,
    signal: AbortSignal
  ) => Promise<string | undefined>,
  timeoutMs = 30_000
) {
  const current = binding('session-node-current', 'session-current', 1n);
  const installed: BindingIdentity[] = [];
  const replies: Array<{
    readonly terminal: number;
    readonly code: number;
    readonly tail?: unknown;
  }> = [];
  const actor = {
    context: { spotId: 'user-spot-a' },
    ...(callback === undefined ? {} : { onDuplicateBind: callback })
  };
  const state = {
    meshName: 'actor-mesh',
    actorId: 'actor-a',
    nativeActorRef: { nodeRid: 'actor-node', actorId: 'actor-a', generation: 1n },
    actor,
    locationGeneration: 7n,
    isMoving: false,
    boundSession: current,
    installBoundSessionBinding(value: BindingIdentity) {
      installed.push(value);
      this.boundSession = value;
    }
  };
  const gateCalls: unknown[][] = [];
  const errors: unknown[] = [];
  const host = Object.assign(Object.create(ZLinkFrameworkRuntimeHost.prototype), {
    options: { registration: { sessionReplacementCallbackTimeoutMs: timeoutMs } },
    actorManager: { getState: () => state },
    spotNodeRuntime: {
      meshNode: () => ({
        status: () => ({ routingId: 'actor-node', lifecycleGeneration: 4n })
      })
    },
    boundSessionRelay: {
      boundSessions: {
        resolveRemoteBoundSessionTarget: (sessionNodeRid: string, sessionRid: string) => ({
          sessionNodeRid,
          sessionRid,
          sessionOwnerNodeGeneration: 6n
        })
      }
    },
    spotManager: {
      executeActorTurn: (...args: unknown[]) => {
        gateCalls.push(args);
        return (args.at(-1) as () => Promise<void>)();
      }
    },
    runtimeOrPreStartErrorSink: {
      reportRuntimeTaskException(_name: string, error: unknown) {
        errors.push(error);
      }
    }
  }) as unknown as {
    dispatchMeshActorBinding(
      meshName: string,
      record: ReceiveRecord,
      signal?: AbortSignal
    ): Promise<void>;
  };
  const record = {
    kind: ReceiveKind.ActorBinding,
    domain: ReadyDomain.Infrastructure,
    sourceNodeRid: 'session-node-incoming',
    sourceNodeGeneration: 6n,
    sourceSpotId: null,
    sourceBindingGeneration: 0n,
    sourceActor: null,
    operationId: { high: 1n, low: 1n },
    operationKind: 1,
    channelName: null,
    topic: null,
    applicationMetadata: null,
    kindData: {
      kind: 'actorBinding',
      transition: 'active',
      actor: { nodeRid: 'actor-node', actorId: 'actor-a', generation: 1n },
      actorNodeGeneration: 4n,
      authorityOwnerGeneration: 7n,
      ownerLeaseGeneration: 1n,
      sessionNodeRid: 'session-node-incoming',
      sessionOwnerNodeGeneration: 6n,
      sessionOwnerId: 'incoming-owner',
      sessionOwnerLeaseGeneration: 2n,
      sessionRid: 'session-incoming',
      bindingGeneration: 2n
    },
    terminalResult: 0,
    failureErrno: 0,
    parts: [],
    reply() {
      replies.push({ terminal: RequestResult.Ok, code: 0 });
      return SubmitResult.Ok;
    },
    replyFailure(terminal: number, code: number, tail?: unknown) {
      replies.push({ terminal, code, tail });
      return SubmitResult.Ok;
    },
    replyActorJoin() {
      return SubmitResult.Ok;
    }
  } as unknown as ReceiveRecord;
  return { host, state, current, installed, replies, gateCalls, record, errors };
}

test('duplicate bind acceptance commits through the Actor gate', async () => {
  let callbackIdentity: readonly unknown[] | undefined;
  const fixture = makeFixture(async (current, incoming) => {
    callbackIdentity = [current, incoming];
    return undefined;
  });

  await fixture.host.dispatchMeshActorBinding('actor-mesh', fixture.record);

  assert.equal(fixture.replies.at(-1)?.terminal, RequestResult.Ok);
  assert.equal(fixture.installed.length, 1);
  assert.equal(fixture.state.boundSession?.sessionRid, 'session-incoming');
  assert.deepEqual(callbackIdentity, [
    {
      sessionOwnerNodeRid: 'session-node-current',
      sessionOwnerNodeGeneration: 11n,
      bindingGeneration: 1n
    },
    {
      sessionOwnerNodeRid: 'session-node-incoming',
      sessionOwnerNodeGeneration: 6n,
      bindingGeneration: 2n
    }
  ]);
  assert.equal(fixture.gateCalls[0]?.[0], 'actor-mesh');
  assert.equal(fixture.gateCalls[0]?.[1], 'user-spot-a');
});

test('duplicate bind rejection preserves current and returns the content message', async () => {
  const fixture = makeFixture(async () => 'already signed in');

  await fixture.host.dispatchMeshActorBinding('actor-mesh', fixture.record);

  assert.equal(fixture.replies.at(-1)?.terminal, RequestResult.Rejected);
  assert.deepEqual(fixture.replies.at(-1)?.tail, {
    kind: 'streamBindRejected',
    rejectionMessage: 'already signed in'
  });
  assert.equal(fixture.installed.length, 0);
  assert.equal(fixture.state.boundSession, fixture.current);
});

test('duplicate bind accepts when the callback returns an empty string', async () => {
  const fixture = makeFixture(async () => '');

  await fixture.host.dispatchMeshActorBinding('actor-mesh', fixture.record);

  assert.equal(fixture.replies.at(-1)?.terminal, RequestResult.Ok);
  assert.equal(fixture.installed.length, 1);
  assert.equal(fixture.state.boundSession?.sessionRid, 'session-incoming');
});

test('duplicate bind timeout rejects and ignores a late acceptance', async () => {
  let release!: (value: string | undefined) => void;
  let callbackSignal: AbortSignal | undefined;
  const fixture = makeFixture(async (_current, _incoming, signal) => {
    callbackSignal = signal;
    return await new Promise<string | undefined>((resolve) => {
      release = resolve;
    });
  }, 10);

  await fixture.host.dispatchMeshActorBinding('actor-mesh', fixture.record);

  assert.equal(fixture.replies.at(-1)?.terminal, RequestResult.TimedOut);
  assert.equal(callbackSignal?.aborted, true);
  assert.equal(fixture.installed.length, 0);
  assert.equal(fixture.state.boundSession, fixture.current);
  release(undefined);
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(fixture.installed.length, 0);
  assert.equal(fixture.state.boundSession, fixture.current);
});

test('duplicate bind without a callback preserves later-Session-wins behavior', async () => {
  const fixture = makeFixture();

  await fixture.host.dispatchMeshActorBinding('actor-mesh', fixture.record);

  assert.equal(fixture.replies.at(-1)?.terminal, RequestResult.Ok);
  assert.equal(fixture.installed.length, 1);
  assert.equal(fixture.state.boundSession?.sessionRid, 'session-incoming');
});

const sameNodeCases = (
  JSON.parse(
    readFileSync('../../runtime/conformance/bound-session-relocation-v1.json', 'utf8')
  ) as {
    sameNodeDuplicateBind: Array<{
      name: string;
      rejectionMessage: string;
      callbackCount: number;
      accepted: boolean;
      replacementCount: number;
    }>;
  }
).sameNodeDuplicateBind;
for (const scenario of sameNodeCases) {
  const reject = !scenario.accepted;
  test(`same-node physical Session bind runs the Actor decision and preserves rejection: ${reject}`, async () => {
    let callbacks = 0;
    const fixture = makeFixture(async () => {
      callbacks += 1;
      return scenario.rejectionMessage || undefined;
    });
    Object.assign(fixture.state, { boundSession: undefined, locationGeneration: 1n });
    const queue = new ApplicationJobQueue(
      resolveApplicationJobQueueConfiguration({ maxQueuedApplicationJobs: 2048n }, () => 1n)
    );
    const errors: unknown[] = [];
    const raw = {
      setServiceIngress() {},
      reserveLocalIngress: async () =>
        ApplicationIngressRecordOwner.create(queue, await queue.acquire(undefined, 'local'), {
          close() {}
        }),
      topology: { peer: () => undefined },
      mailbox: {
        tryEnqueue(value: unknown) {
          const record = value as {
            stateful: {
              kindData: unknown;
              reply(terminal: number, code: number, payload?: unknown, tail?: unknown): boolean;
            };
            applicationJob: { close(): void };
          };
          void fixture.host
            .dispatchMeshActorBinding('actor-mesh', {
              kind: ReceiveKind.ActorBinding,
              kindData: record.stateful.kindData,
              reply: () =>
                record.stateful.reply(RequestResult.Ok, 0)
                  ? SubmitResult.Ok
                  : SubmitResult.Terminated,
              replyFailure: (terminal: number, code: number, tail: unknown) =>
                record.stateful.reply(terminal, code, undefined, tail)
                  ? SubmitResult.Ok
                  : SubmitResult.Terminated
            } as unknown as ReceiveRecord)
            .catch((error) => errors.push(error))
            .finally(() => record.applicationJob.close());
          return true;
        }
      }
    } as unknown as RawServiceMeshRuntime;
    const runtime = new ServiceStatefulRuntime(raw, 'actor-node', 4n);
    runtime.setSessionOwnerDescriptors({
      local: () => ({ ownerId: 'local-owner', leaseGeneration: 9n }),
      peer: async () => undefined
    });
    const actor = runtime.createActor('actor-a').ref;
    Object.assign(fixture.state, { nativeActorRef: actor });
    let replacementCount = 0;
    let replaced!: () => void;
    const replacement = new Promise<void>((resolve) => {
      replaced = resolve;
    });
    try {
      const first = await runtime.bindSession(
        'session-current',
        actor,
        1000,
        async () => true,
        () => {
          replacementCount += 1;
          replaced();
        }
      ).promise;
      assert.equal(first.terminalResult, RequestResult.Ok);
      assert.equal(callbacks, 0);
      const current = runtime.registry.binding(actor);
      assert.ok(current);
      const incoming = await runtime.bindSession('session-incoming', actor, 1000, async () => true)
        .promise;
      assert.equal(callbacks, scenario.callbackCount);
      if (reject) {
        assert.equal(incoming.terminalResult, RequestResult.Rejected);
        assert.equal(runtime.registry.binding(actor), current);
        assert.equal(fixture.state.boundSession.sessionRid, 'session-current');
      } else {
        assert.equal(incoming.terminalResult, RequestResult.Ok);
        await replacement;
        assert.equal(runtime.registry.binding(actor)?.sessionRid, 'session-incoming');
        assert.equal(fixture.state.boundSession.sessionRid, 'session-incoming');
      }
      assert.equal(replacementCount, scenario.replacementCount);
      assert.deepEqual(errors, []);
    } finally {
      runtime.close();
    }
  });
}

for (const kind of ['first', 'duplicate', 'idempotent'] as const) {
  test(`session bound notification commits before callback and reply: ${kind}`, async () => {
    const fixture = makeFixture(async () => undefined);
    if (kind === 'first') Object.assign(fixture.state, { boundSession: undefined });
    if (kind === 'idempotent')
      Object.assign(fixture.state, {
        boundSession: {
          ...fixture.current,
          sessionNodeRid: 'session-node-incoming',
          sessionRid: 'session-incoming',
          sessionOwnerNodeGeneration: 6n
        }
      });
    const calls: unknown[] = [];
    Object.assign(fixture.state.actor, {
      onSessionBound(incoming: unknown, signal: AbortSignal) {
        assert.equal(fixture.installed.length, 1);
        assert.equal(fixture.replies.length, 1);
        assert.equal(signal.aborted, false);
        calls.push(incoming);
      }
    });
    await fixture.host.dispatchMeshActorBinding('actor-mesh', fixture.record);
    assert.deepEqual(calls, [
      {
        sessionOwnerNodeRid: 'session-node-incoming',
        sessionOwnerNodeGeneration: 6n,
        bindingGeneration: 2n
      }
    ]);
    assert.equal(fixture.gateCalls.length, 1);
    assert.equal(fixture.replies.length, 1);
    assert.deepEqual(fixture.errors, []);
  });
}

test('rejected bind does not notify', async () => {
  const fixture = makeFixture(async () => 'rejected');
  let calls = 0;
  Object.assign(fixture.state.actor, {
    onSessionBound() {
      calls++;
    }
  });
  await fixture.host.dispatchMeshActorBinding('actor-mesh', fixture.record);
  assert.equal(calls, 0);
  assert.equal(fixture.replies[0]?.terminal, RequestResult.Rejected);
});

for (const asynchronous of [false, true]) {
  test(`bound notification failure is reported once without changing bind: async=${asynchronous}`, async () => {
    const fixture = makeFixture();
    const failure = new Error('notification failed');
    Object.assign(fixture.state.actor, {
      onSessionBound() {
        if (asynchronous) return Promise.reject(failure);
        throw failure;
      }
    });
    await fixture.host.dispatchMeshActorBinding('actor-mesh', fixture.record);
    assert.deepEqual(fixture.errors, [failure]);
    assert.equal(fixture.replies.length, 1);
    assert.equal(fixture.replies[0]?.terminal, RequestResult.Ok);
    assert.equal(fixture.installed.length, 1);
  });
}

for (const cancel of [false, true]) {
  test(`bound notification cancellation preserves bind and reports only timeout: shutdown=${cancel}`, async (t) => {
    t.mock.timers.enable({ apis: ['setTimeout'] });
    const fixture = makeFixture(undefined, 100);
    const lifecycle = new AbortController();
    let callbackSignal: AbortSignal | undefined;
    let entered!: () => void;
    const started = new Promise<void>((resolve) => {
      entered = resolve;
    });
    let release!: () => void;
    Object.assign(fixture.state.actor, {
      async onSessionBound(_incoming: unknown, signal: AbortSignal) {
        callbackSignal = signal;
        entered();
        await new Promise<void>((resolve) => {
          release = resolve;
        });
        throw new Error('late callback failure');
      }
    });
    const dispatch = fixture.host.dispatchMeshActorBinding(
      'actor-mesh',
      fixture.record,
      lifecycle.signal
    );
    // A missing callback must fail deterministically instead of awaiting an unfulfilled promise.
    assert.equal(fixture.gateCalls.length, 1);
    await started;
    if (cancel) lifecycle.abort();
    // Allow the callback-start continuation to arm its existing timeout.
    await Promise.resolve();
    t.mock.timers.tick(100);
    await dispatch;
    assert.equal(callbackSignal?.aborted, true);
    assert.equal(fixture.errors.length, cancel ? 0 : 1);
    assert.equal(fixture.replies.length, 1);
    assert.equal(fixture.replies[0]?.terminal, RequestResult.Ok);
    release();
    await new Promise((resolve) => setImmediate(resolve));
    assert.equal(fixture.errors.length, cancel ? 0 : 1);
  });
}

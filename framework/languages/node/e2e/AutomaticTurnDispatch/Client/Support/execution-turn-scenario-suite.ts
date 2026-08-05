import type { ZlinkStreamConnector } from '@zlink-systems/stream-connector';
import type {
  ActorAwaitReq,
  ActorAwaitRes,
  ActorFastReq,
  ActorJoinAwaitReq,
  AwaitCancelMsg,
  AwaitEvidenceReq,
  AwaitEvidenceRes,
  AwaitEvidenceWaitReq,
  AwaitMsg,
  AwaitReq,
  AwaitTimeoutMsg,
  AutomaticTurnDispatchRes,
  BindAwaitActorsReq,
  BindAwaitActorsRes,
  CounterAwaitMsg,
  CounterReadReq,
  CounterReadRes,
  CounterResetMsg,
  CpuWorkerAwaitMsg,
  DeferredJoinFailureMsg,
  EnsureSpotReq,
  EnsureSpotRes,
  HttpAwaitMsg,
  IoWorkerBatchReq,
  IoWorkerBatchRes,
  ProbeMsg,
  ProbeReq,
  RemoteSpotAwaitReq,
  SelfCycleMsg,
  SelfSendMsg,
  TimerStartMsg,
  TimerStopMsg
} from '../../Shared/messages';
import { AutomaticTurnDispatchNames } from '../../Shared/messages';
import { containsRequestMarkersInOrder, ensure } from './scenario-assert';

interface AwaitActorScenarioContext {
  readonly spotId: string;
  readonly actorA: string;
  readonly actorB: string;
}

type Terminator = 'async' | 'yield';

export class ExecutionTurnScenarioSuite {
  private spotId?: string;
  private actors?: AwaitActorScenarioContext;

  constructor(private readonly client: ZlinkStreamConnector) {}

  async tdA1(): Promise<void> {
    const spot = await this.spot();
    const asyncId = newId('TD-A1-async');
    const asyncReply = await this.spotRequest<AutomaticTurnDispatchRes>(spot, {
      requestId: asyncId,
      delayMs: 50,
      correlationId: 'TD-A1',
      terminator: 'async'
    } satisfies AwaitReq, 'AwaitReq');
    ensure(asyncReply.marker === 'async-completed', 'TD-A1 async terminator did not complete.');

    const yieldId = newId('TD-A1-yield');
    await this.sendSpot(spot, {
      requestId: yieldId,
      delayMs: 50,
      correlationId: 'TD-A1',
      terminator: 'yield'
    } satisfies AwaitMsg, 'AwaitMsg');
    await this.evidence(yieldId, 'yield-completed');
  }

  async tdA2(): Promise<void> { await this.verifySpotInterleave('TD-A2', 'async', false); }

  async tdA3(): Promise<void> { await this.verifyCounter('TD-A3', 'async', 8); }

  async tdA4(): Promise<void> {
    const spot = await this.spot();
    const requestId = newId('TD-A4');
    const reply = await this.spotRequest<AutomaticTurnDispatchRes>(spot, {
      requestId,
      delayMs: 1000,
      correlationId: 'completion-axis',
      terminator: 'async'
    } satisfies AwaitReq, 'AwaitReq');
    ensure(reply.marker === 'async-completed', 'TD-A4 async completion did not resume.');
  }

  async tdA5(): Promise<void> { await this.verifyTimerInterleave('TD-A5', 'async', false); }

  async tdB1(): Promise<void> { await this.verifySpotInterleave('TD-B1', 'yield', true); }

  async tdB2(): Promise<void> {
    const spot = await this.spot();
    const requestId = newId('TD-B2');
    await this.sendSpot(spot, {
      requestId,
      delayMs: 300,
      correlationId: 'queue-order',
      terminator: 'yield'
    } satisfies AwaitMsg, 'AwaitMsg');
    await this.evidence(requestId, 'yield-released');
    for (let index = 1; index <= 3; index += 1) {
      await this.sendSpot(spot, { requestId, marker: `probe-${index}` } satisfies ProbeMsg, 'ProbeMsg');
    }
    const evidence = await this.evidence(requestId, 'yield-completed');
    containsRequestMarkersInOrder(evidence, requestId, [
      'yield-released', 'marker=probe-1', 'marker=probe-2', 'marker=probe-3', 'yield-resumed', 'yield-completed'
    ], 'TD-B2 continuation queue order mismatch.');
  }

  async tdB3(): Promise<void> { await this.verifyCounter('TD-B3', 'yield', 1); }

  async tdB4(): Promise<void> { await this.verifyTimerInterleave('TD-B4', 'yield', true); }

  async tdC1(): Promise<void> { await this.verifyHttpInterleave('TD-C1', 'yield', true); }

  async tdC2(): Promise<void> { await this.verifyHttpInterleave('TD-C2', 'async', false); }

  async tdC3(): Promise<void> {
    const spot = await this.spot();
    const requestId = newId('TD-C3');
    const result = await this.spotRequest<IoWorkerBatchRes>(spot, {
      requestId,
      count: 32,
      delayMs: 100
    } satisfies IoWorkerBatchReq, 'IoWorkerBatchReq');
    ensure(result.completed === 32, `TD-C3 completed ${result.completed} of 32 I/O workers.`);
    const deadline = Date.now() + 5000;
    let evidence: readonly string[] = [];
    do {
      evidence = await this.evidenceSnapshot(requestId);
      if (countMatching(evidence, requestId, 'io-worker-completed') === 32) break;
      await delay(25);
    } while (Date.now() < deadline);
    ensure(countMatching(evidence, requestId, 'io-worker-completed') === 32,
      'TD-C3 did not complete all 32 I/O workers.');
    ensure(evidence.every((line) => !line.includes('WorkerQueueFull')),
      'TD-C3 exhausted the CPU worker queue with I/O work.');
  }

  async tdC4(): Promise<void> {
    await this.verifyCpuWorker('TD-C4-async', 'async', false);
    await this.verifyCpuWorker('TD-C4-yield', 'yield', true);
  }

  async tdC5(): Promise<void> {
    // The runner performs the source-level blocking-I/O gate. This live probe
    // proves that the accepted CPU-worker path still completes afterwards.
    await this.verifyCpuWorker('TD-C5', 'yield', true);
  }

  async tdD1(): Promise<void> { await this.verifyActorInterleave('TD-D1', false); }

  async tdD2(): Promise<void> { await this.verifyActorInterleave('TD-D2', true); }

  async tdD3(): Promise<void> {
    const spot = await this.spot();
    const requestId = newId('TD-D3');
    await this.sendSpot(spot, {
      requestId,
      timerName: requestId,
      mode: 'yield-then-next',
      periodMs: 40,
      delayMs: 250
    } satisfies TimerStartMsg, 'TimerStartMsg');
    const evidence = await this.evidence(requestId, 'timer-next-completed');
    containsRequestMarkersInOrder(evidence, requestId, [
      'timer-yield-released', 'timer-yield-resumed', 'timer-yield-completed', 'timer-next-started'
    ], 'TD-D3 timer reentered before its yielded continuation completed.');
    await this.sendSpot(spot, { requestId } satisfies TimerStopMsg, 'TimerStopMsg');
  }

  async tdD4(): Promise<void> {
    const perActorSpot = `execution-turn-per-actor-${uniqueId()}`;
    await this.ensureSpot(perActorSpot, 'play-a', 'per_actor');
    const actors = await this.createActorContext(perActorSpot);
    await this.ensureActorInSpot(actors.actorA, actors.spotId, 'TD-D4-join-a');
    await this.ensureActorInSpot(actors.actorB, actors.spotId, 'TD-D4-join-b');
    const actorRequestId = newId('TD-D4-actor-a');
    const actorPending = this.actorRequest<ActorAwaitRes>(actors.actorA, {
      requestId: actorRequestId,
      delayMs: 5000,
      terminator: 'async'
    } satisfies ActorAwaitReq, 'ActorAwaitReq');
    await this.evidence(actorRequestId, 'actor-await-held');

    const actorASecondRequestId = newId('TD-D4-actor-a-second');
    const actorASecond = this.actorRequest<ActorAwaitRes>(actors.actorA, {
      requestId: actorASecondRequestId,
      marker: 'actor-a-fast'
    } satisfies ActorFastReq, 'ActorFastReq');

    const timerRequests = [
      { requestId: newId('TD-D4-timer-a'), timerName: newId('timer-a') },
      { requestId: newId('TD-D4-timer-b'), timerName: newId('timer-b') }
    ];
    let timersStopped = false;
    const stopTimers = async (): Promise<void> => {
      if (timersStopped) return;
      timersStopped = true;
      for (const timer of timerRequests) {
        await this.sendSpot(actors.spotId, { requestId: timer.requestId } satisfies TimerStopMsg, 'TimerStopMsg');
      }
    };
    for (const timer of timerRequests) {
      await this.sendSpot(actors.spotId, {
        requestId: timer.requestId,
        timerName: timer.timerName,
        mode: 'fast',
        periodMs: 40,
        delayMs: 0
      } satisfies TimerStartMsg, 'TimerStartMsg');
      await delay(25);
    }

    const actorBRequestId = newId('TD-D4-actor-b');
    const actorB = this.actorRequest<ActorAwaitRes>(actors.actorB, {
      requestId: actorBRequestId,
      marker: 'actor-b-fast'
    } satisfies ActorFastReq, 'ActorFastReq');
    try {
      await Promise.all([actorB, ...timerRequests.map(async (timer) => {
        const evidence = await this.evidence(timer.requestId, 'timer-fast-completed');
        ensure(evidence.some((line) => line.includes(`request=${timer.requestId}`)),
          `TD-D4 timer '${timer.timerName}' did not complete.`);
      })]);

      const evidenceBeforeAResumes = await this.evidenceSnapshot(actorRequestId);
      ensure(!evidenceBeforeAResumes.some((line) => line.includes('actor-await-resumed')),
        'TD-D4 Actor A resumed before the independent lanes were checked.');
      await stopTimers();

      await Promise.all([actorPending, actorASecond]);
      const actorEvidence = await this.evidenceSnapshot(actorRequestId);
      const actorResumedIndex = actorEvidence.findIndex((line) => line.includes('actor-await-resumed'));
      const actorBCompletedIndex = actorEvidence.findIndex((line) =>
        line.includes(`request=${actorBRequestId}`) && line.includes('actor-fast-completed'));
      ensure(actorBCompletedIndex >= 0 && actorBCompletedIndex < actorResumedIndex,
        'TD-D4 Actor B did not complete before Actor A resumed.');
      for (const timer of timerRequests) {
        const timerCompletedIndex = actorEvidence.findIndex((line) =>
          line.includes(`request=${timer.requestId}`) && line.includes('timer-fast-completed'));
        ensure(timerCompletedIndex >= 0 && timerCompletedIndex < actorResumedIndex,
          `TD-D4 timer '${timer.timerName}' did not complete before Actor A resumed.`);
      }
      const actorACompletedIndex = actorEvidence.findIndex((line) =>
        line.includes(`request=${actorRequestId}`) && line.includes('actor-await-completed'));
      const actorASecondStartedIndex = actorEvidence.findIndex((line) =>
        line.includes(`request=${actorASecondRequestId}`) && line.includes('actor-fast-started'));
      ensure(actorACompletedIndex >= 0 && actorASecondStartedIndex > actorACompletedIndex,
        'TD-D4 Actor A second request re-entered before the first request completed.');
    } finally {
      await stopTimers();
    }
  }

  async tdD5(): Promise<void> {
    const actors = await this.createActorContext(await this.spot());
    const yieldRequestId = newId('TD-D5-yield');
    let failure: unknown;
    try {
      await this.actorRequest<ActorAwaitRes>(actors.actorA, {
        requestId: yieldRequestId,
        delayMs: 100,
        terminator: 'yield'
      } satisfies ActorAwaitReq, 'ActorAwaitReq');
    } catch (error) {
      failure = error;
    }
    ensure(failure !== undefined, 'TD-D5 unsupported Actor Yield completed unexpectedly.');
    const failureError = typeof failure === 'object' && failure !== null && 'error' in failure
      ? (failure as { error?: unknown }).error
      : undefined;
    const failureMessage = typeof failureError === 'string'
      ? failureError
      : failureError !== undefined
        ? JSON.stringify(failureError)
        : failure instanceof Error ? failure.message : String(failure);
    ensure(/yield|invalid.?operation/i.test(failureMessage),
      `TD-D5 failure did not identify the unsupported Yield context: ${failureMessage}`);
    const evidence = await this.evidenceSnapshot(yieldRequestId);
    ensure(!evidence.some((line) => line.includes(`request=${yieldRequestId}`)
      && (line.includes('actor-await-resumed') || line.includes('actor-await-completed'))),
    'TD-D5 unsupported Yield unexpectedly resumed the Actor operation.');

    const asyncRequestId = newId('TD-D5-async');
    const asyncReply = await this.actorRequest<ActorAwaitRes>(actors.actorA, {
      requestId: asyncRequestId,
      delayMs: 20,
      terminator: 'async'
    } satisfies ActorAwaitReq, 'ActorAwaitReq');
    ensure(asyncReply.marker === 'actor-await-completed',
      'TD-D5 Async contrast did not complete.');
  }

  async tdD6(): Promise<void> {
    const spot = await this.spot();
    for (const terminator of ['async', 'yield'] as const) {
      const requestId = newId(`TD-D6-${terminator}`);
      await this.sendSpot(spot, {
        requestId,
        timeoutMs: 1000,
        terminator
      } satisfies SelfCycleMsg, 'SelfCycleMsg');
      const evidence = await this.evidence(requestId, 'self-cycle-rejected');
      ensure(!evidence.some((line) => line.includes(`request=${requestId}`)
        && line.includes('self-cycle-unexpected-completed')),
      `TD-D6 ${terminator} self-request completed unexpectedly.`);
    }

    const sendRequestId = newId('TD-D6-send');
    await this.sendSpot(spot, {
      requestId: sendRequestId,
      marker: 'self-send'
    } satisfies SelfSendMsg, 'SelfSendMsg');
    const sendEvidence = await this.evidence(sendRequestId, 'probe-completed');
    containsRequestMarkersInOrder(sendEvidence, sendRequestId, [
      'self-send-started', 'self-send-completed', 'probe-completed'
    ], 'TD-D6 self-send FIFO order mismatch.');
  }

  async tdE1(): Promise<void> {
    const actors = await this.createActorContext(await this.spot());
    const reply = await this.actorRequest<ActorAwaitRes>(actors.actorA, {
      requestId: newId('TD-E1'),
      targetSpotId: actors.spotId
    } satisfies ActorJoinAwaitReq, 'ActorJoinAwaitReq');
    ensure(reply.marker === 'actor-join-await-completed', 'TD-E1 Entry-to-user Spot join failed.');
  }

  async tdE2(): Promise<void> {
    const actors = await this.createActorContext(await this.spot());
    await this.ensureActorInSpot(actors.actorA, actors.spotId, 'TD-E2-prepare');
    const target = `td-e2-target-${uniqueId()}`;
    await this.ensureSpot(target, 'play-a');
    const reply = await this.actorRequest<ActorAwaitRes>(actors.actorA, {
      requestId: newId('TD-E2'),
      targetSpotId: target
    } satisfies ActorJoinAwaitReq, 'ActorJoinAwaitReq');
    ensure(reply.marker === 'actor-join-completed', 'TD-E2 user-to-user Spot join failed.');
  }

  async tdE3(): Promise<void> {
    const actors = await this.createActorContext(await this.spot());
    const spotA = `td-e3-a-${uniqueId()}`;
    const spotB = `td-e3-b-${uniqueId()}`;
    await this.ensureSpot(spotA, 'play-a');
    await this.ensureSpot(spotB, 'play-a');
    await this.ensureActorInSpot(actors.actorA, spotA, 'TD-E3-prepare-a');
    await this.ensureActorInSpot(actors.actorB, spotB, 'TD-E3-prepare-b');
    const replies = await Promise.all([
      this.actorRequest<ActorAwaitRes>(actors.actorA, {
        requestId: newId('TD-E3-A'), targetSpotId: spotB
      } satisfies ActorJoinAwaitReq, 'ActorJoinAwaitReq'),
      this.actorRequest<ActorAwaitRes>(actors.actorB, {
        requestId: newId('TD-E3-B'), targetSpotId: spotA
      } satisfies ActorJoinAwaitReq, 'ActorJoinAwaitReq')
    ]);
    ensure(replies.every((reply) => reply.marker === 'actor-join-completed'),
      'TD-E3 opposite joins did not both complete.');
  }

  async tdE2A(): Promise<void> {
    const actors = await this.createActorContext(await this.spot());
    await this.ensureActorInSpot(actors.actorA, actors.spotId, 'TD-E2A-prepare-a');
    await this.ensureActorInSpot(actors.actorB, actors.spotId, 'TD-E2A-prepare-b');

    for (const failureMode of ['exception', 'cancel'] as const) {
      const requestId = newId(`TD-E2A-${failureMode}`);
      const firstTargetSpotId = `td-e2a-${failureMode}-a-${uniqueId()}`;
      const secondTargetSpotId = `td-e2a-${failureMode}-b-${uniqueId()}`;
      await this.ensureSpot(firstTargetSpotId, 'play-a');
      await this.ensureSpot(secondTargetSpotId, 'play-a');
      let failure: unknown;
      try {
        await this.actorRequest<ActorAwaitRes>(actors.actorA, {
          requestId,
          firstActorId: actors.actorA,
          secondActorId: actors.actorB,
          firstTargetSpotId,
          secondTargetSpotId,
          failureMode
        } satisfies DeferredJoinFailureMsg, 'DeferredJoinFailureMsg');
      } catch (error) {
        failure = error;
      }
      ensure(failure !== undefined, `TD-E2A ${failureMode} handler completed unexpectedly.`);
      await this.evidence(requestId, 'deferred-join-failure-registered');

      const probe = await this.actorRequest<ActorAwaitRes>(actors.actorA, {
        requestId,
        marker: `${failureMode}-source-still-member`
      } satisfies ActorFastReq, 'ActorFastReq');
      ensure(probe.marker === `${failureMode}-source-still-member`,
        `TD-E2A ${failureMode} source Actor did not remain usable.`);
      const evidence = await this.evidence(requestId, 'actor-fast-completed');
      ensure(!evidence.some((line) => line.includes(`request=${requestId}`)
        && (line.includes(`spot=${firstTargetSpotId}`) || line.includes(`spot=${secondTargetSpotId}`))
        && (line.includes('actor-joined') || line.includes('actor-admitted'))),
      `TD-E2A ${failureMode} unexpectedly started a deferred Join.`);
    }
  }

  async tdF1(): Promise<void> {
    const owner = await this.spot();
    const target = `td-f1-target-${uniqueId()}`;
    await this.ensureSpot(target, 'play-b');
    for (const terminator of ['async', 'yield'] as const) {
      const reply = await this.spotRequest<AutomaticTurnDispatchRes>(owner, {
        requestId: newId(`TD-F1-${terminator}`),
        targetSpotId: target,
        delayMs: 100,
        terminator
      } satisfies RemoteSpotAwaitReq, 'RemoteSpotAwaitReq');
      ensure(reply.nodeRid === 'play-a', `TD-F1 ${terminator} continuation left the caller node.`);
    }
  }

  async tdF2(): Promise<void> { await this.verifySpotInterleave('TD-F2', 'yield', true, 'play-b'); }

  async tdF3(): Promise<void> { await this.verifyActorInterleave('TD-F3', false); }

  async tdF4(): Promise<void> {
    for (const terminator of ['async', 'yield'] as const) await this.verifyTimeoutRecovery(terminator);
  }

  async tdF5(): Promise<void> {
    for (const terminator of ['async', 'yield'] as const) await this.verifyCancellationRecovery(terminator);
  }

  async tdF5A(): Promise<void> {
    const spot = await this.spot();
    const requestId = newId('TD-F5A-preflight');
    const reply = await this.spotRequest<AutomaticTurnDispatchRes>(spot, {
      requestId,
      marker: 'shutdown-preflight'
    } satisfies ProbeReq, 'ProbeReq');
    ensure(reply.marker === 'shutdown-preflight', 'TD-F5A shutdown preflight probe failed.');
  }

  async tdF6(): Promise<void> {
    const spot = await this.spot();
    const requestId = newId('TD-F6');
    await this.sendSpot(spot, { requestId, timeoutMs: 150 } satisfies SelfCycleMsg, 'SelfCycleMsg');
    await this.evidence(requestId, 'self-cycle-rejected');
    const reply = await this.spotRequest<AutomaticTurnDispatchRes>(spot, {
      requestId,
      marker: 'post-cycle'
    } satisfies ProbeReq, 'ProbeReq');
    ensure(reply.marker === 'post-cycle', 'TD-F6 Spot did not recover after the wait-for cycle timeout.');
  }

  async tdG1(): Promise<void> {
    await this.tdA1();
    await this.verifySpotInterleave('TD-G1-async', 'async', false);
    await this.verifySpotInterleave('TD-G1-yield', 'yield', true);
  }

  private async verifySpotInterleave(
    scenarioId: string,
    terminator: Terminator,
    probeDuringWait: boolean,
    targetNode = 'play-a'
  ): Promise<void> {
    const spot = targetNode === 'play-a' ? await this.spot() : `${scenarioId.toLowerCase()}-${uniqueId()}`;
    if (targetNode !== 'play-a') await this.ensureSpot(spot, targetNode);
    const requestId = newId(scenarioId);
    await this.sendSpot(spot, {
      requestId,
      delayMs: 300,
      correlationId: scenarioId,
      terminator
    } satisfies AwaitMsg, 'AwaitMsg');
    await this.evidence(requestId, terminator === 'yield' ? 'yield-released' : 'async-held', targetNode);
    await this.sendSpot(spot, { requestId, marker: 'interleave-probe' } satisfies ProbeMsg, 'ProbeMsg');
    await this.evidence(requestId, `${terminator}-completed`, targetNode);
    const evidence = await this.evidence(requestId, 'probe-completed', targetNode);
    containsRequestMarkersInOrder(evidence, requestId, probeDuringWait
      ? ['yield-released', 'probe-started', 'probe-completed', 'yield-resumed']
      : ['async-held', 'async-resumed', 'async-completed', 'probe-started', 'probe-completed'],
    `${scenarioId} Spot interleave order mismatch.`);
  }

  private async verifyCounter(scenarioId: string, terminator: Terminator, expected: number): Promise<void> {
    const spot = await this.spot();
    const requestId = newId(scenarioId);
    await this.sendSpot(spot, { requestId } satisfies CounterResetMsg, 'CounterResetMsg');
    await this.evidence(requestId, 'counter-reset');
    for (let index = 0; index < 8; index += 1) {
      await this.sendSpot(spot, {
        requestId,
        operationId: `op-${index}`,
        delayMs: terminator === 'yield' ? 2000 : 100,
        terminator
      } satisfies CounterAwaitMsg, 'CounterAwaitMsg');
      await this.evidence(requestId, `operation=op-${index}|observed=`);
    }
    await this.evidence(requestId, `operation=op-7|value=${expected}`);
    const counter = await this.spotRequest<CounterReadRes>(spot, { requestId } satisfies CounterReadReq, 'CounterReadReq');
    ensure(counter.value === expected, `${scenarioId} expected counter ${expected}, actual ${counter.value}.`);
  }

  private async verifyTimerInterleave(
    scenarioId: string,
    terminator: Terminator,
    tickDuringWait: boolean
  ): Promise<void> {
    const spot = await this.spot();
    const requestId = newId(scenarioId);
    await this.sendSpot(spot, {
      requestId,
      timerName: requestId,
      mode: 'fast',
      periodMs: 40,
      delayMs: 0
    } satisfies TimerStartMsg, 'TimerStartMsg');
    await this.evidence(requestId, 'timer-started');
    await this.sendSpot(spot, {
      requestId,
      delayMs: 300,
      correlationId: scenarioId,
      terminator
    } satisfies AwaitMsg, 'AwaitMsg');
    await this.evidence(requestId, terminator === 'yield' ? 'yield-released' : 'async-held');
    await this.evidence(requestId, `${terminator}-completed`);
    const evidence = await this.evidence(requestId, 'timer-fast-completed');
    containsRequestMarkersInOrder(evidence, requestId, tickDuringWait
      ? ['yield-released', 'timer-fast-started', 'timer-fast-completed', 'yield-resumed']
      : ['async-held', 'async-completed', 'timer-fast-started', 'timer-fast-completed'],
    `${scenarioId} timer interleave order mismatch.`);
    await this.sendSpot(spot, { requestId } satisfies TimerStopMsg, 'TimerStopMsg');
  }

  private async verifyHttpInterleave(
    scenarioId: string,
    terminator: Terminator,
    probeDuringWait: boolean
  ): Promise<void> {
    const spot = await this.spot();
    const requestId = newId(scenarioId);
    await this.sendSpot(spot, { requestId, delayMs: 300, terminator } satisfies HttpAwaitMsg, 'HttpAwaitMsg');
    await this.evidence(requestId, `http-${terminator}-${probeDuringWait ? 'released' : 'held'}`);
    await this.sendSpot(spot, { requestId, marker: 'http-probe' } satisfies ProbeMsg, 'ProbeMsg');
    await this.evidence(requestId, `http-${terminator}-completed`);
    const evidence = await this.evidence(requestId, 'probe-completed');
    containsRequestMarkersInOrder(evidence, requestId, probeDuringWait
      ? ['http-yield-released', 'probe-started', 'probe-completed', 'http-yield-resumed']
      : ['http-async-held', 'http-async-completed', 'probe-started', 'probe-completed'],
    `${scenarioId} HTTP interleave order mismatch.`);
  }

  private async verifyCpuWorker(
    scenarioId: string,
    terminator: Terminator,
    probeDuringWait: boolean
  ): Promise<void> {
    const spot = await this.spot();
    const requestId = newId(scenarioId);
    await this.sendSpot(spot, { requestId, delayMs: 250, terminator } satisfies CpuWorkerAwaitMsg, 'CpuWorkerAwaitMsg');
    await this.evidence(requestId, `cpu-worker-${terminator}-${probeDuringWait ? 'released' : 'held'}`);
    await this.sendSpot(spot, { requestId, marker: 'cpu-probe' } satisfies ProbeMsg, 'ProbeMsg');
    await this.evidence(requestId, `cpu-worker-${terminator}-completed`);
    const evidence = await this.evidence(requestId, 'probe-completed');
    containsRequestMarkersInOrder(evidence, requestId, probeDuringWait
      ? ['cpu-worker-yield-released', 'probe-started', 'probe-completed', 'cpu-worker-yield-completed']
      : ['cpu-worker-async-held', 'cpu-worker-async-completed', 'probe-started', 'probe-completed'],
    `${scenarioId} CPU worker turn order mismatch.`);
    const completion = evidence.find((line) => line.includes(`request=${requestId}`)
      && line.includes('cpu-worker') && line.includes('completed'));
    ensure(completion !== undefined && /worker-thread=[1-9][0-9]*/.test(completion),
      `${scenarioId} did not execute on a CPU worker thread.`);
  }

  private async verifyActorInterleave(scenarioId: string, sameActor: boolean): Promise<void> {
    const actors = await this.actorContext();
    const requestId = newId(scenarioId);
    const pending = this.actorRequest<ActorAwaitRes>(actors.actorA, {
      requestId,
      delayMs: 300,
      terminator: 'yield'
    } satisfies ActorAwaitReq, 'ActorAwaitReq');
    await this.evidence(requestId, 'actor-await-released');
    const fast = this.actorRequest<ActorAwaitRes>(sameActor ? actors.actorA : actors.actorB, {
      requestId,
      marker: 'actor-fast'
    } satisfies ActorFastReq, 'ActorFastReq');
    await Promise.all([pending, fast]);
    const evidence = await this.evidence(requestId, 'actor-fast-completed');
    containsRequestMarkersInOrder(evidence, requestId, sameActor
      ? ['actor-await-released', 'actor-await-resumed', 'actor-await-completed', 'actor-fast-started']
      : ['actor-await-released', 'actor-fast-started', 'actor-fast-completed', 'actor-await-resumed'],
    `${scenarioId} actor mailbox order mismatch.`);
  }

  private async verifyTimeoutRecovery(terminator: Terminator): Promise<void> {
    const spot = await this.spot();
    const requestId = newId(`TD-F4-${terminator}`);
    await this.sendSpot(spot, {
      requestId,
      delayMs: 700,
      timeoutMs: 100,
      terminator
    } satisfies AwaitTimeoutMsg, 'AwaitTimeoutMsg');
    await this.evidence(requestId, 'timeout-await-completed');
    await this.sendSpot(spot, { requestId, marker: 'timeout-probe' } satisfies ProbeMsg, 'ProbeMsg');
    const evidence = await this.evidence(requestId, 'probe-completed');
    ensure(!evidence.some((line) => line.includes(`request=${requestId}`)
      && line.includes('timeout-await-unexpected-resumed')),
    `TD-F4 ${terminator} call resumed after timeout.`);
  }

  private async verifyCancellationRecovery(terminator: Terminator): Promise<void> {
    const spot = await this.spot();
    const requestId = newId(`TD-F5-${terminator}`);
    await this.sendSpot(spot, {
      requestId,
      delayMs: 800,
      cancelAfterMs: 100,
      terminator
    } satisfies AwaitCancelMsg, 'AwaitCancelMsg');
    await this.evidence(requestId, 'cancel-await-completed');
    await this.sendSpot(spot, { requestId, marker: 'cancel-probe' } satisfies ProbeMsg, 'ProbeMsg');
    const evidence = await this.evidence(requestId, 'probe-completed');
    ensure(!evidence.some((line) => line.includes(`request=${requestId}`)
      && line.includes('cancel-await-unexpected-resumed')),
    `TD-F5 ${terminator} call resumed after cancellation.`);
  }

  private async spot(): Promise<string> {
    if (this.spotId !== undefined) return this.spotId;
    this.spotId = `execution-turn-${uniqueId()}`;
    await this.ensureSpot(this.spotId, 'play-a');
    return this.spotId;
  }

  private async actorContext(): Promise<AwaitActorScenarioContext> {
    if (this.actors !== undefined) return this.actors;
    this.actors = await this.createActorContext(await this.spot());
    return this.actors;
  }

  private async createActorContext(spotId: string): Promise<AwaitActorScenarioContext> {
    const actorA = `actor-a-${uniqueId()}`;
    const actorB = `actor-b-${uniqueId()}`;
    const result = await this.client.request({ spotId, actorIds: [actorA, actorB] } satisfies BindAwaitActorsReq)
      .packetName('BindAwaitActorsReq').timeout(30000).submit<BindAwaitActorsRes>();
    ensure(result.actors.length === 2, 'Execution turn actor binding failed.');
    return { spotId, actorA, actorB };
  }

  private async ensureActorInSpot(actorId: string, spotId: string, scenarioId: string): Promise<void> {
    const reply = await this.actorRequest<ActorAwaitRes>(actorId, {
      requestId: newId(scenarioId), targetSpotId: spotId
    } satisfies ActorJoinAwaitReq, 'ActorJoinAwaitReq');
    ensure(reply.marker.includes('actor-join'), `${scenarioId} actor placement failed.`);
    const deadline = Date.now() + 30000;
    const joinedMarker = `actor-joined|rid=play-a|spot=${spotId}|actor=${actorId}`;
    while (Date.now() < deadline) {
      const evidence = await this.evidenceSnapshot(reply.requestId);
      if (evidence.some((line) => line.includes(joinedMarker))) return;
      await delay(25);
    }
    throw new Error(`${scenarioId} actor '${actorId}' did not join Spot '${spotId}'.`);
  }

  private async ensureSpot(
    spotId: string,
    targetNode: string,
    executionMode?: EnsureSpotReq['executionMode']
  ): Promise<void> {
    const builder = this.client.request({ spotId, executionMode } satisfies EnsureSpotReq).packetName('EnsureSpotReq');
    if (targetNode !== 'play-a') builder.metadata(AutomaticTurnDispatchNames.targetNodeRidMetadata, targetNode);
    const result = await builder.timeout(30000).submit<EnsureSpotRes>();
    ensure(result.spotId === spotId, `Spot creation failed for '${spotId}'.`);
    ensure(result.nodeRid === targetNode, `Spot '${spotId}' was created on '${result.nodeRid}', expected '${targetNode}'.`);
    const requestId = newId('route-ready');
    const deadline = Date.now() + 30000;
    while (true) {
      try {
        const reply = await this.client.request({ requestId, marker: 'route-ready' } satisfies ProbeReq)
          .packetName('ProbeReq')
          .metadata(AutomaticTurnDispatchNames.spotIdMetadata, spotId)
          .timeout(5000).submit<AutomaticTurnDispatchRes>();
        ensure(reply.marker === 'route-ready', `Spot route probe for '${spotId}' returned the wrong marker.`);
        return;
      } catch (error) {
        const message = error instanceof Error ? error.message : String(error);
        if (!/Host unreachable|timed out|not connected|disconnected/i.test(message) || Date.now() >= deadline) {
          throw new Error(`Spot route for '${spotId}' did not become ready: ${message}`);
        }
        await delay(50);
      }
    }
  }

  private async sendSpot<T>(spotId: string, message: T, packetName: string): Promise<void> {
    await this.client.send(message).packetName(packetName)
      .metadata(AutomaticTurnDispatchNames.spotIdMetadata, spotId).submit();
  }

  private async spotRequest<T>(spotId: string, request: object, packetName: string): Promise<T> {
    return await this.client.request(request).packetName(packetName)
      .metadata(AutomaticTurnDispatchNames.spotIdMetadata, spotId)
      .timeout(30000).submit<T>();
  }

  private async actorRequest<T>(actorId: string, request: object, packetName: string): Promise<T> {
    return await this.client.request(request).packetName(packetName)
      .metadata(AutomaticTurnDispatchNames.actorIdMetadata, actorId)
      .timeout(30000).submit<T>();
  }

  private async evidence(requestId: string, marker: string, targetNode = 'play-a'): Promise<readonly string[]> {
    const result = await this.client.request({
      requestId, marker, timeoutMilliseconds: 30000
    } satisfies AwaitEvidenceWaitReq).packetName('AwaitEvidenceWaitReq')
      .metadata(AutomaticTurnDispatchNames.targetNodeRidMetadata, targetNode)
      .timeout(30000).submit<AwaitEvidenceRes>();
    return result.evidence;
  }

  private async evidenceSnapshot(requestId: string): Promise<readonly string[]> {
    const result = await this.client.request({ requestId } satisfies AwaitEvidenceReq)
      .packetName('AwaitEvidenceReq')
      .metadata(AutomaticTurnDispatchNames.targetNodeRidMetadata, 'play-a')
      .timeout(30000).submit<AwaitEvidenceRes>();
    return result.evidence;
  }

}

function countMatching(evidence: readonly string[], requestId: string, marker: string): number {
  return evidence.filter((line) => line.includes(`request=${requestId}`) && line.includes(marker)).length;
}

function newId(prefix: string): string { return `${prefix}-${uniqueId()}`; }

function uniqueId(): string { return `${Date.now().toString(16)}-${Math.random().toString(16).slice(2)}`; }

function delay(milliseconds: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

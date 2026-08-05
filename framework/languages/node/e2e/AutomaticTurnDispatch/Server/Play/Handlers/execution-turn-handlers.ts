import { Inject, Injectable } from '@nestjs/common';
import {
  ZLinkPacket,
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException,
  type ZLinkMessageContext,
  type ZLinkSpotManager,
  type ZLinkSpotPacketHandler,
  type ZLinkSpotRequestHandler
} from '@zlink-systems/framework';
import {
  ZLINK_SPOT_MANAGER,
  type ZLinkServerHttpClient,
  zlinkHttpClientToken
} from '@zlink-systems/nestjs';
import type {
  CounterAwaitMsg,
  CounterReadReq,
  CounterReadRes,
  CounterResetMsg,
  CpuWorkerAwaitMsg,
  ExternalDelayRes,
  HttpAwaitMsg,
  IoWorkerBatchReq,
  IoWorkerBatchRes,
  SelfCycleMsg,
  SelfSendMsg
} from '../../../Shared/messages';
import { ProbeReq } from '../../../Shared/messages';
import { EvidenceStore } from '../Support/evidence-store';
import type { AwaitProbeSpot } from '../Spots/await-probe-spot';

@Injectable()
@ZLinkPacket('CounterResetMsg')
export class CounterResetHandler implements ZLinkSpotPacketHandler<AwaitProbeSpot, CounterResetMsg> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(spot: AwaitProbeSpot, request: CounterResetMsg, context: ZLinkMessageContext): Promise<void> {
    void context;
    spot.resetCounter();
    this.evidence.add(`counter-reset|rid=${this.evidence.rid}|spot=${spot.context.spotId}|request=${request.requestId}`);
  }
}

@Injectable()
@ZLinkPacket('CounterAwaitMsg')
export class CounterAwaitHandler implements ZLinkSpotPacketHandler<AwaitProbeSpot, CounterAwaitMsg> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(spot: AwaitProbeSpot, request: CounterAwaitMsg, context: ZLinkMessageContext): Promise<void> {
    void context;
    const observed = spot.readCounter();
    this.evidence.add(
      `counter-${request.terminator}-started|rid=${this.evidence.rid}|spot=${spot.context.spotId}`
      + `|request=${request.requestId}|operation=${request.operationId}|observed=${observed}`
    );
    const call = spot.context.runIoWorker(async (signal) => {
      await delay(request.delayMs, signal);
      return request.operationId;
    });
    if (request.terminator === 'yield') {
      await call.yield();
    } else {
      await call.submit();
    }
    spot.writeCounter(observed + 1);
    this.evidence.add(
      `counter-${request.terminator}-completed|rid=${this.evidence.rid}|spot=${spot.context.spotId}`
      + `|request=${request.requestId}|operation=${request.operationId}|value=${spot.readCounter()}`
    );
  }
}

@Injectable()
@ZLinkPacket('CounterReadReq')
export class CounterReadHandler implements ZLinkSpotRequestHandler<AwaitProbeSpot, CounterReadReq, CounterReadRes> {
  async handle(spot: AwaitProbeSpot, request: CounterReadReq, context: ZLinkMessageContext): Promise<CounterReadRes> {
    void context;
    return { requestId: request.requestId, value: spot.readCounter() };
  }
}

@Injectable()
@ZLinkPacket('HttpAwaitMsg')
export class HttpAwaitHandler implements ZLinkSpotPacketHandler<AwaitProbeSpot, HttpAwaitMsg> {
  constructor(
    @Inject(zlinkHttpClientToken('external-api')) private readonly client: ZLinkServerHttpClient,
    private readonly evidence: EvidenceStore
  ) {}

  async handle(spot: AwaitProbeSpot, request: HttpAwaitMsg, context: ZLinkMessageContext): Promise<void> {
    void context;
    this.evidence.add(
      `http-${request.terminator}-started|rid=${this.evidence.rid}|spot=${spot.context.spotId}`
      + `|request=${request.requestId}`
    );
    const call = this.client.get('/delay')
      .query('requestId', request.requestId)
      .query('marker', request.terminator)
      .query('delayMs', String(request.delayMs));
    this.evidence.add(
      `http-${request.terminator}-${request.terminator === 'yield' ? 'released' : 'held'}`
      + `|rid=${this.evidence.rid}|spot=${spot.context.spotId}|request=${request.requestId}`
    );
    const response = request.terminator === 'yield'
      ? await call.yield<ExternalDelayRes>()
      : await call.async<ExternalDelayRes>();
    this.evidence.add(
      `http-${request.terminator}-resumed|rid=${this.evidence.rid}|spot=${spot.context.spotId}`
      + `|request=${request.requestId}|marker=${response.body.marker}`
    );
    this.evidence.add(
      `http-${request.terminator}-completed|rid=${this.evidence.rid}|spot=${spot.context.spotId}`
      + `|request=${request.requestId}`
    );
  }
}

@Injectable()
@ZLinkPacket('IoWorkerBatchReq')
export class IoWorkerBatchHandler implements ZLinkSpotRequestHandler<AwaitProbeSpot, IoWorkerBatchReq, IoWorkerBatchRes> {
  constructor(
    @Inject(zlinkHttpClientToken('external-api')) private readonly client: ZLinkServerHttpClient,
    private readonly evidence: EvidenceStore
  ) {}

  async handle(
    spot: AwaitProbeSpot,
    request: IoWorkerBatchReq,
    context: ZLinkMessageContext
  ): Promise<IoWorkerBatchRes> {
    void context;
    const calls = Array.from({ length: request.count }, (_unused, index) => {
      const operationId = `io-${index.toString().padStart(2, '0')}`;
      this.evidence.add(
        `io-worker-started|rid=${this.evidence.rid}|spot=${spot.context.spotId}`
        + `|request=${request.requestId}|operation=${operationId}`
      );
      const call = spot.context.runIoWorker(async () => {
        const response = await this.client.get('/delay')
          .query('requestId', request.requestId)
          .query('marker', operationId)
          .query('delayMs', String(request.delayMs))
          .async<ExternalDelayRes>();
        return response.body;
      });
      this.evidence.add(
        `io-worker-released|rid=${this.evidence.rid}|spot=${spot.context.spotId}`
        + `|request=${request.requestId}|operation=${operationId}`
      );
      return { operationId, call };
    });
    const pending = calls.map(({ call }, index) => index === calls.length - 1 ? call.yield() : call.submit());
    const results = await Promise.all(calls.map(async ({ operationId }, index) => {
      const result = await pending[index];
      this.evidence.add(
        `io-worker-completed|rid=${this.evidence.rid}|spot=${spot.context.spotId}`
        + `|request=${request.requestId}|operation=${operationId}|marker=${result.marker}`
      );
      return result;
    }));
    return { requestId: request.requestId, completed: results.length };
  }
}

@Injectable()
@ZLinkPacket('CpuWorkerAwaitMsg')
export class CpuWorkerAwaitHandler implements ZLinkSpotPacketHandler<AwaitProbeSpot, CpuWorkerAwaitMsg> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(spot: AwaitProbeSpot, request: CpuWorkerAwaitMsg, context: ZLinkMessageContext): Promise<void> {
    void context;
    this.evidence.add(
      `cpu-worker-${request.terminator}-started|rid=${this.evidence.rid}|spot=${spot.context.spotId}`
      + `|request=${request.requestId}`
    );
    const call = spot.context.runCpuWorker(() => {
      const startedAt = Date.now();
      while (Date.now() - startedAt < 250) {
        // The bounded CPU worker intentionally remains busy for the probe window.
      }
      return require('node:worker_threads').threadId as number;
    });
    this.evidence.add(
      `cpu-worker-${request.terminator}-${request.terminator === 'yield' ? 'released' : 'held'}`
      + `|rid=${this.evidence.rid}|spot=${spot.context.spotId}|request=${request.requestId}`
    );
    const workerThread = request.terminator === 'yield' ? await call.yield() : await call.submit();
    this.evidence.add(
      `cpu-worker-${request.terminator}-completed|rid=${this.evidence.rid}|spot=${spot.context.spotId}`
      + `|request=${request.requestId}|worker-thread=${workerThread}`
    );
  }
}

@Injectable()
@ZLinkPacket('SelfCycleMsg')
export class SelfCycleHandler implements ZLinkSpotPacketHandler<AwaitProbeSpot, SelfCycleMsg> {
  constructor(
    @Inject(ZLINK_SPOT_MANAGER) private readonly spotHandles: ZLinkSpotManager,
    private readonly evidence: EvidenceStore
  ) {}

  async handle(spot: AwaitProbeSpot, request: SelfCycleMsg, context: ZLinkMessageContext): Promise<void> {
    void context;
    const self = await this.spotHandles.find(String(spot.context.spotId));
    if (self === undefined) throw new Error(`Self SpotHandle was not resolved for '${spot.context.spotId}'.`);
    try {
      const call = spot.context.outbound
        .requestToSpot(self.spotId, Object.assign(new ProbeReq(), { requestId: request.requestId, marker: 'cycle' }))
        .timeout(request.timeoutMs);
      if (request.terminator === 'yield') {
        await call.yield();
      } else {
        await call.submit();
      }
      this.evidence.add(`self-cycle-unexpected-completed|request=${request.requestId}`);
    } catch (error) {
      if (error instanceof ZLinkFrameworkException
        && error.kind === ZLinkFrameworkErrorKind.InvalidOperation) {
        this.evidence.add(
          `self-cycle-rejected|rid=${this.evidence.rid}|spot=${spot.context.spotId}`
          + `|request=${request.requestId}|terminator=${request.terminator ?? 'async'}`
        );
        return;
      }
      const name = error instanceof Error ? error.name : 'Error';
      this.evidence.add(
        `self-cycle-timed-out|rid=${this.evidence.rid}|spot=${spot.context.spotId}`
        + `|request=${request.requestId}|error=${name}`
      );
    }
  }
}

@Injectable()
@ZLinkPacket('SelfSendMsg')
export class SelfSendHandler implements ZLinkSpotPacketHandler<AwaitProbeSpot, SelfSendMsg> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(spot: AwaitProbeSpot, request: SelfSendMsg, context: ZLinkMessageContext): Promise<void> {
    void context;
    this.evidence.add(
      `self-send-started|rid=${this.evidence.rid}|spot=${spot.context.spotId}|request=${request.requestId}`
    );
    await spot.context.outbound.sendToSpot(spot.context.spotId, Object.assign(new ProbeReq(), {
      requestId: request.requestId,
      marker: request.marker
    })).submit();
    this.evidence.add(
      `self-send-completed|rid=${this.evidence.rid}|spot=${spot.context.spotId}|request=${request.requestId}`
    );
  }
}

function delay(delayMs: number, signal: AbortSignal): Promise<void> {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(resolve, delayMs);
    const onAbort = () => {
      clearTimeout(timer);
      reject(signal.reason instanceof Error ? signal.reason : new Error('I/O worker canceled.'));
    };
    if (signal.aborted) {
      onAbort();
      return;
    }
    signal.addEventListener('abort', onAbort, { once: true });
  });
}

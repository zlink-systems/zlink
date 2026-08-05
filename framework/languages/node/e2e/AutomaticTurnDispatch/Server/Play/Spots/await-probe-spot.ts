import { Injectable, Scope } from '@nestjs/common';
import type {
  ZLinkMessage,
  ZLinkSpot,
  ZLinkSpotActorJoinResult,
  ZLinkSpotContext
} from '@zlink-systems/framework';
import type { DelayReq } from '../../../Shared/messages';
import { EvidenceStore } from '../Support/evidence-store';
import { HoldCommandHandler, ProbeCommandHandler, ProbeRequestHandler, WorkerAwaitCommandHandler, AwaitCommandHandler, AwaitRequestHandler } from '../Handlers/basic-spot-handlers';
import {
  CounterAwaitHandler,
  CounterReadHandler,
  CounterResetHandler,
  CpuWorkerAwaitHandler,
  HttpAwaitHandler,
  IoWorkerBatchHandler,
  SelfCycleHandler,
  SelfSendHandler
} from '../Handlers/execution-turn-handlers';
import {
  AwaitCancelCommandHandler,
  AwaitTimeoutCommandHandler
} from '../Handlers/failure-spot-handlers';
import { RemoteSpotAwaitCommandHandler, RemoteSpotAwaitHandler } from '../Handlers/remote-spot-handlers';
import { TimerStartCommandHandler, TimerStopCommandHandler } from '../Handlers/timer-spot-handlers';
import { SpotActorFastHandler, SpotActorFastSendHandler, SpotActorJoinAwaitHandler, SpotActorPushAwaitHandler, SpotActorAwaitHandler, AwaitActor } from './await-actors';
import { AwaitTimerState } from './await-timer-state';

@Injectable({ scope: Scope.TRANSIENT })
export class AwaitProbeSpot implements ZLinkSpot<AwaitActor> {
  readonly context!: ZLinkSpotContext<AwaitActor, AwaitProbeSpot>;
  private readonly timers = new Map<string, AwaitTimerState>();
  private readonly actors = new Map<string, AwaitActor>();
  private counter = 0;

  constructor(private readonly evidence: EvidenceStore) {}

  configure(): void {
    this.context.handlers.addPacket(HoldCommandHandler);
    this.context.handlers.addPacket(AwaitCommandHandler);
    this.context.handlers.addPacket(AwaitRequestHandler);
    this.context.handlers.addPacket(WorkerAwaitCommandHandler);
    this.context.handlers.addPacket(AwaitTimeoutCommandHandler);
    this.context.handlers.addPacket(AwaitCancelCommandHandler);
    this.context.handlers.addPacket(ProbeCommandHandler);
    this.context.handlers.addPacket(ProbeRequestHandler);
    this.context.handlers.addPacket(CounterResetHandler);
    this.context.handlers.addPacket(CounterAwaitHandler);
    this.context.handlers.addPacket(CounterReadHandler);
    this.context.handlers.addPacket(HttpAwaitHandler);
    this.context.handlers.addPacket(IoWorkerBatchHandler);
    this.context.handlers.addPacket(CpuWorkerAwaitHandler);
    this.context.handlers.addPacket(SelfCycleHandler);
    this.context.handlers.addPacket(SelfSendHandler);
    this.context.handlers.addPacket(RemoteSpotAwaitHandler);
    this.context.handlers.addPacket(RemoteSpotAwaitCommandHandler);
    this.context.handlers.addPacket(TimerStartCommandHandler);
    this.context.handlers.addPacket(TimerStopCommandHandler);
  }

  async onActorJoin(actorId: string, request: ZLinkMessage): Promise<ZLinkSpotActorJoinResult> {
    const delay = request.decode<DelayReq>(Object as never);
    if (delay.delayMs > 0) {
      await new Promise<void>((resolve, reject) => {
        const timeout = setTimeout(resolve, delay.delayMs);
        void reject;
      });
    }
    this.evidence.add(`actor-admitted|rid=${this.evidence.rid}|spot=${this.context.spotId}|actor=${actorId}`);
    return { accepted: true, reply: delay };
  }

  async onJoinedActor(actor: AwaitActor): Promise<void> {
    this.actors.set(actor.actorId, actor);
    this.evidence.add(`actor-joined|rid=${this.evidence.rid}|spot=${this.context.spotId}|actor=${actor.actorId}`);
  }

  async onLeaveActor(actor: AwaitActor): Promise<void> {
    this.actors.delete(actor.actorId);
    this.evidence.add(`actor-left|rid=${this.evidence.rid}|spot=${this.context.spotId}|actor=${actor.actorId}`);
  }

  async onDisconnectActor(actor: AwaitActor): Promise<void> { void actor; }

  readCounter(): number { return this.counter; }

  writeCounter(value: number): void { this.counter = value; }

  resetCounter(): void { this.counter = 0; }

  findActor(actorId: string): AwaitActor | undefined {
    return this.actors.get(actorId);
  }

  tryAddTimerState(state: AwaitTimerState): boolean {
    if (this.timers.has(state.timerName)) {
      return false;
    }
    this.timers.set(state.timerName, state);
    return true;
  }

  findTimerState(timerName: string): AwaitTimerState | undefined {
    return this.timers.get(timerName);
  }

  stopScenarioTimers(requestId: string): void {
    const matches = [...this.timers.values()].filter((state) => state.requestId === requestId);
    for (const state of matches) {
      this.timers.delete(state.timerName);
    }
    void Promise.all(matches.map((state) => state.timer?.cancel())).catch(() => undefined);
  }
}

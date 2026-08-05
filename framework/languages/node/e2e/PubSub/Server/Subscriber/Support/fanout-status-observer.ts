import type { ZLinkFanoutRuntime, ZLinkObservedStatus } from '@zlink-systems/framework';
import { EvidenceStore } from '../Infrastructure/evidence-store';

export class FanoutStatusObserverProbe {
  private readonly controllers = new Set<AbortController>();
  private readonly tasks = new Set<Promise<void>>();
  private slowRelease?: () => void;
  private slowController?: AbortController;
  private slowTask?: Promise<void>;

  constructor(
    private readonly runtime: ZLinkFanoutRuntime | undefined,
    private readonly evidence: EvidenceStore,
    private readonly channelName = 'events'
  ) {}

  startSlow(): void {
    if (this.runtime === undefined || this.slowTask !== undefined) return;
    const controller = new AbortController();
    this.controllers.add(controller);
    this.slowController = controller;
    let release!: () => void;
    const gate = new Promise<void>((resolve) => { release = resolve; });
    this.slowRelease = release;
    const task = this.observe('slow', controller.signal, gate);
    this.slowTask = task;
    this.tasks.add(task);
    void task.finally(() => {
      this.tasks.delete(task);
      this.controllers.delete(controller);
      if (this.slowTask === task) this.slowTask = undefined;
    });
  }

  startNormal(): void {
    if (this.runtime === undefined) return;
    const controller = new AbortController();
    this.controllers.add(controller);
    const task = this.observe('normal', controller.signal);
    this.tasks.add(task);
    void task.finally(() => {
      this.tasks.delete(task);
      this.controllers.delete(controller);
    });
  }

  releaseSlow(): void {
    this.slowRelease?.();
    this.slowRelease = undefined;
  }

  async cancelSlow(): Promise<void> {
    this.releaseSlow();
    this.slowController?.abort();
    await this.slowTask;
  }

  async stop(): Promise<void> {
    this.releaseSlow();
    for (const controller of this.controllers) controller.abort();
    await Promise.allSettled([...this.tasks]);
    this.controllers.clear();
    this.tasks.clear();
    this.slowController = undefined;
    this.slowTask = undefined;
  }

  private async observe(
    label: 'slow' | 'normal',
    signal: AbortSignal,
    gate?: Promise<void>
  ): Promise<void> {
    try {
      for await (const observed of this.runtime!.observe(this.channelName, 1, signal)) {
        this.record(label, observed);
        if (gate !== undefined) {
          await gate;
          gate = undefined;
        }
      }
    } catch (error) {
      if (!signal.aborted) {
        this.evidence.add(
          `fanout-observer-error|observer=${label}|message=${error instanceof Error ? error.message : String(error)}`
        );
      }
    }
  }

  private record(
    label: 'slow' | 'normal',
    observed: ZLinkObservedStatus<Awaited<ReturnType<ZLinkFanoutRuntime['snapshot']>>>
  ): void {
    const status = observed.status;
    this.evidence.add(
      `fanout-observer-${label}|sequence=${status.sequence.toString()}`
      + `|ready=${status.readyPublisherCount}`
      + `|coalesced=${observed.loss.coalescedCount.toString()}`
    );
  }
}

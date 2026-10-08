import type { ZLinkActorManager, ZLinkRouteMeshRuntime } from '@zlink-systems/framework';
import { PerfClock } from '../shared/clock';
import { RoleConfig } from '../shared/contracts';
import { PERF_ACTOR_TYPE } from '../server-support/actor-echo-support';
import { until } from '../server-support/wait';

// §10.9 and §10.10 preparation: one unbound Actor per logical stream, created through the public manager during
// setup (§4). The create is never retried; only the public RouteMesh status says when the Actor node is a ready peer.
export class ActorCallerSetup {
  constructor(
    private readonly config: RoleConfig,
    private readonly actors: ZLinkActorManager,
    private readonly mesh: ZLinkRouteMeshRuntime
  ) {}

  // Returns the create result as evidence; the caller reports objectsReady once its probes have also finished.
  async createActors(signal: AbortSignal): Promise<unknown> {
    const { config } = this;
    await until(
      () => this.mesh.snapshot(config.meshName!).readyPeerCount > 0,
      config.workload.setupTimeoutMs,
      'a ready Actor Object Server peer'
    );
    let created = 0;
    let existing = 0;
    let totalNs = 0n;
    let maxNs = 0n;
    let next = 0;
    // §5: --connect-concurrency bounds the source role's object preparation.
    const worker = async (): Promise<void> => {
      for (;;) {
        const index = next++;
        if (index >= config.actorIds.length) return;
        const started = PerfClock.now();
        const result = await this.actors
          .getOrCreate(config.actorIds[index], PERF_ACTOR_TYPE)
          .inMesh(config.meshName!)
          .timeout(config.workload.setupTimeoutMs)
          .submit(signal);
        const elapsed = PerfClock.now() - started;
        if (result.status === 'created') created++;
        else if (result.status === 'existing') existing++;
        else throw new Error(`Actor '${config.actorIds[index]}' creation was rejected.`);
        totalNs += elapsed;
        if (elapsed > maxNs) maxNs = elapsed;
      }
    };
    await Promise.all(
      Array.from(
        { length: Math.min(config.workload.connectConcurrency as number, config.actorIds.length) },
        worker
      )
    );
    return {
      kind: 'actorCreate',
      source: 'ZLinkActorManager.getOrCreate.inMesh.submit',
      observedValue: {
        created,
        existing,
        expectedActors: config.actorIds.length,
        createMeanMs: Number(totalNs) / 1e6 / config.actorIds.length,
        createMaxMs: Number(maxNs) / 1e6
      }
    };
  }
}

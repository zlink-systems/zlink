import {
  ZLinkUserSpotExecutionMode,
  type ZLinkRouteMeshRuntime,
  type ZLinkSpot,
  type ZLinkSpotActorJoinResult,
  type ZLinkSpotContext,
  type ZLinkSpotManager,
  type ZLinkUserSpotFactoryBuilder
} from '@zlink-systems/framework';
import { RoleConfig } from '../shared/contracts';
import { Measurement } from '../shared/measurement';
import { FrameworkBuilder, ObjectsReadiness } from '../server-support/server-application';

// What every Spot role of §10.3-§10.8 shares: an automatic RouteMesh Object Server hosting this cell's User Spots as
// Actor-less SpotWide Spots (§10), and their creation through the public manager as setup. No call that a scenario
// measures lives here; each scenario file shows its own requests, sends and Yield/worker calls.
export const SPOT_TYPE = 'perf-spot';

// The membership callbacks every User Spot must declare; these Spots admit no Actor (Actor count is 0, §10.7).
export abstract class ActorlessSpot implements ZLinkSpot {
  readonly context!: ZLinkSpotContext;
  async onActorJoin(): Promise<ZLinkSpotActorJoinResult> {
    return { accepted: false };
  }
  async onJoinedActor(): Promise<void> {}
  async onLeaveActor(): Promise<void> {}
}

// callsChannel: the Spot handler calls this cell's ChannelName (§10.5, §10.6), so the node registers a Channel client.
export function configureSpotRole(
  builder: FrameworkBuilder,
  config: RoleConfig,
  callsChannel: boolean,
  spotType: new () => ZLinkSpot
): void {
  const mesh = builder
    .addRouteMesh(config.meshName!)
    .setRoutingIdPrefix('perf-spot')
    .listen(config.transportEndpoints.mesh)
    .setAdvertiseHost('127.0.0.1');
  if (callsChannel) mesh.channel(config.channelName!).client();
  mesh
    .objects()
    .server()
    .addSpotFactory(
      SPOT_TYPE,
      spotType as never,
      (factory: ZLinkUserSpotFactoryBuilder<ZLinkSpot>) =>
        factory
          .executionMode(ZLinkUserSpotExecutionMode.SpotWide)
          .stableTypeLimit(config.spotIds.length)
          .disableRelocation()
    );
}

export interface SpotObjects {
  ready: boolean;
  evidence: unknown[];
}

// Setup: every SpotId of the cell is created through the public manager. Not part of any measured latency.
// A role that probes reports objectsReady only after its probes, so warmup never overlaps a probe.
export async function createSpots(
  config: RoleConfig,
  manager: ZLinkSpotManager,
  mesh: ZLinkRouteMeshRuntime,
  measurement: Measurement
): Promise<SpotObjects | undefined> {
  const signal = AbortSignal.timeout(config.workload.setupTimeoutMs);
  try {
    const created: unknown[] = [];
    for (const spotId of config.spotIds) {
      const result = await manager
        .getOrCreate(spotId, SPOT_TYPE)
        .timeout(config.workload.setupTimeoutMs)
        .submit(signal);
      if (result.state === 'rejected') throw new Error(`Spot ${spotId} was rejected.`);
      created.push({ spotId, state: result.state, meshName: result.spot.meshName });
    }
    const placement = mesh.snapshot(config.meshName!).placement;
    return {
      ready: placement.isAvailable && placement.activeSpotCount >= config.spotIds.length,
      evidence: [
        {
          kind: 'spotCreate',
          source: 'ZLinkSpotManager.getOrCreate.submit',
          observedValue: created
        },
        {
          kind: 'spotPlacement',
          source: 'ZLinkRouteMeshRuntime.snapshot.placement',
          observedValue: {
            isAvailable: placement.isAvailable,
            activeSpotCount: placement.activeSpotCount,
            expectedSpots: config.spotIds.length
          }
        }
      ]
    };
  } catch (error) {
    measurement.recordDiagnostic(error);
    return undefined;
  }
}

export function publishSpots(readiness: ObjectsReadiness, objects: SpotObjects): void {
  readiness.set(
    objects.ready,
    'The Object Server has not activated every User Spot of this cell.',
    objects.evidence
  );
}

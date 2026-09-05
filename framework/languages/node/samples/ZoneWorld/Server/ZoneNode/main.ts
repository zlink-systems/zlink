import 'reflect-metadata';
import * as fs from 'node:fs';
import { setTimeout as delay } from 'node:timers/promises';
import { NestFactory } from '@nestjs/core';
import {
  ZLINK_ACTOR_CLIENT,
  ZLINK_ACTOR_MANAGER,
  ZLINK_FANOUT_RUNTIME,
  ZLINK_LOCATION_RUNTIME_QUERY,
  ZLINK_ROUTE_MESH_RUNTIME,
  ZLINK_ROUTE_MESH_RUNTIME_OPTIONS,
  ZLINK_SPOT_MANAGER
} from '@zlink-systems/nestjs';
import type {
  ZLinkActorClient,
  ZLinkActorManager,
  ZLinkFanoutRuntime,
  ZLinkLocationRuntimeQuery,
  ZLinkRouteMeshRuntime,
  ZLinkRouteMeshRuntimeOptions,
  ZLinkSpotManager
} from '@zlink-systems/framework';
import { readConfigPath, ZONEWORLD_CONFIG } from '../Configuration/configuration';
import type { ZoneWorldConfiguration } from '../Configuration/configuration';
import { closeRuntime, waitForShutdown } from '../runtime-support';
import { ZoneIds, ZoneWorldNames, ZoneWorldSpec } from '../../Shared/spec';
import { createZoneNodeModule } from './zone-node-module';
import { EnterWorldReq, EnterWorldRes } from '../../Shared/contracts';
import { MaintenanceStore } from '../Configuration/maintenance-store';
import { NodeRuntimeState } from './Domain/node-runtime-state';
import { botRoutes } from './Domain/bot-patrol';
import { adjacentZones } from './Domain/world';
import { ZoneSpot } from './Infrastructure/ZLink/Spots/zone-spot';
import { OpsReportAdapter } from './Infrastructure/ZLink/Monitoring/ops-report-adapter';
import type { ZoneId } from '../../Shared/spec';

let statusTimer: NodeJS.Timeout | undefined;
const zoneClaimRetryDelayMs = 250;
const zoneClaimRetryAttempts = 120;
const allowEmptyZoneSetReadyAttempt = 8;

async function bootstrap(): Promise<void> {
  const ZoneNodeModule = createZoneNodeModule(hasConfiguredZones());
  const app = await NestFactory.createApplicationContext(ZoneNodeModule, {
    logger: false,
    abortOnError: false
  });
  const config = app.get<ZoneWorldConfiguration>(ZONEWORLD_CONFIG);
  const node = config.zoneNode;
  if (node === undefined) throw new Error('ZoneNode configuration is required.');
  if (node.zoneCapacity > 0) {
    const state = app.get(NodeRuntimeState);
    const maintenance = app.get(MaintenanceStore);
    state.restore(await maintenance.readAll());
    console.log(`maintenance restored node=${node.nodeId} enabled=${state.ownMaintenance()}`);
    if (node.waitForPlacementPeer === true) {
      const routeMeshRuntime = app.get<ZLinkRouteMeshRuntime>(ZLINK_ROUTE_MESH_RUNTIME, { strict: false });
      await waitForPlacementPeer(routeMeshRuntime, ZoneWorldNames.zoneMesh);
    }
    if (node.allowEmptyZoneSet === true) {
      await waitForEmptyZoneSet(state, node.nodeId);
    } else {
      await ensureZones(
        app,
        state,
        node.zoneCapacity,
        node.bootstrapZones ?? Object.values(ZoneIds),
      );
    }
    const zones = state.zones();
    if (node.disableBots !== true) {
      await spawnBots(app, zones);
    }
    // Create local startup actors before withdrawing this node from future
    // placement. The weight change must not remove the only eligible target
    // for those actors.
    if (node.placementWeightAfterZoneCreation !== undefined) {
      await updatePlacementWeight(
        app,
        node.nodeId,
        node.placementWeightAfterZoneCreation
      );
    }
    if (node.disableBots !== true) {
      console.log(`bot-start=ready node=${node.nodeId}`);
      // Keep topology and Ops reporting available while bot ticks remain
      // paused. The runner releases the tick gate after normal checks.
      void waitForBotStart(node.botStartSignalPath).then(() => state.enableBotTicks());
    } else {
      state.enableBotTicks();
    }
    const reports = app.get(OpsReportAdapter);
    const report = async () => {
      try {
        await reports.reportNodeStatus();
      } catch {
        // Ops may start after this node; the periodic report retries through the public channel.
      }
    };
    statusTimer = setInterval(() => { void report(); }, ZoneWorldSpec.nodeStatusReportPeriodMs);
    await report();
    console.log(`topology=ready node=${node.nodeId} zones=${zones.join(',')}`);
  } else {
    console.log(`topology=ready node=${node.nodeId} zones=`);
  }
  if (node.zoneCapacity === 0) {
    const fanout = app.get<ZLinkFanoutRuntime>(ZLINK_FANOUT_RUNTIME, { strict: false });
    await waitForFanoutSubscriber(fanout, ZoneWorldNames.broadcastChannel, node.nodeId);
  }
  try {
    await waitForShutdown();
  } finally {
    if (statusTimer !== undefined) clearInterval(statusTimer);
    await closeRuntime(app);
  }
}

async function waitForFanoutSubscriber(
  runtime: ZLinkFanoutRuntime,
  channelName: string,
  nodeId: string
): Promise<void> {
  const deadline = Date.now() + 10_000;
  for (;;) {
    if (runtime.snapshot(channelName).isReady) {
      console.log(`fanout subscriber=ready node=${nodeId}`);
      return;
    }
    if (Date.now() >= deadline) {
      throw new Error(`Fanout subscriber '${channelName}' did not become ready on '${nodeId}'.`);
    }
    await delay(50);
  }
}

function hasConfiguredZones(): boolean {
  const configPath = readConfigPath(process.argv.slice(2));
  const document = JSON.parse(fs.readFileSync(configPath, 'utf8')) as {
    sample?: { zoneNode?: { zoneCapacity?: unknown } };
  };
  const capacity = document.sample?.zoneNode?.zoneCapacity;
  return typeof capacity === 'number' && capacity > 0;
}

bootstrap().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});

export {};

async function waitForBotStart(signalPath: string | undefined): Promise<void> {
  if (signalPath === undefined) return;
  while (!fs.existsSync(signalPath)) await delay(50);
}

async function waitForPlacementPeer(runtime: ZLinkRouteMeshRuntime, meshName: string): Promise<void> {
  while (!runtime.isReady(meshName) || runtime.snapshot(meshName).readyPeerCount < 1) {
    await delay(50);
  }
}

async function updatePlacementWeight(
  app: { get<T>(token: unknown, options?: { strict: boolean }): T },
  nodeId: string,
  weight: number
): Promise<void> {
  const options = app.get<ZLinkRouteMeshRuntimeOptions>(
    ZLINK_ROUTE_MESH_RUNTIME_OPTIONS,
    { strict: false }
  );
  options.mesh(ZoneWorldNames.zoneMesh).placementWeight = weight;
  const locations = app.get<ZLinkLocationRuntimeQuery>(
    ZLINK_LOCATION_RUNTIME_QUERY,
    { strict: false }
  );
  const deadline = Date.now() + 10_000;
  for (;;) {
    const descriptors = await locations.listMeshNodeDescriptors(ZoneWorldNames.zoneMesh);
    if (descriptors.items.some((descriptor) => descriptor.placementWeight === weight)) {
      console.log(`placement weight updated node=${nodeId} weight=${weight}`);
      return;
    }
    if (Date.now() >= deadline) {
      throw new Error(`Placement weight update for '${nodeId}' was not published.`);
    }
    await delay(50);
  }
}

async function spawnBots(app: { get<T>(token: unknown, options?: { strict: boolean }): T }, zones: readonly string[]): Promise<void> {
  const manager = app.get<ZLinkActorManager>(ZLINK_ACTOR_MANAGER, { strict: false });
  const client = app.get<ZLinkActorClient>(ZLINK_ACTOR_CLIENT, { strict: false });
  for (const route of botRoutes.filter((candidate) => zones.includes(candidate.zoneId))) {
    if (await manager.find(route.playerId) !== undefined) continue;
    const result = await manager
      .getOrCreate(route.playerId, ZoneWorldNames.playerActorType)
      .inMesh(ZoneWorldNames.zoneMesh)
      .submit();
    if (result.status === 'rejected') {
      throw new Error(`Bot actor '${route.playerId}' creation was rejected.`);
    }
    const actor = result.actor;
    const entered = await client.requestToActor(
      actor.actorId,
      new EnterWorldReq(route.x, route.y, true, route.dirX, route.dirY)
    ).timeout(10_000).submit<EnterWorldRes>();
    if (entered.error !== null) throw new Error(`Bot '${route.playerId}' could not enter the world: ${entered.error}.`);
    console.log(`bot spawned bot=${route.playerId} zone=${route.zoneId}`);
  }
}

async function ensureZones(
  app: { get<T>(token: unknown, options?: { strict: boolean }): T },
  state: NodeRuntimeState,
  expectedLocalCapacity: number,
  candidates: readonly string[]
): Promise<void> {
  const spots = app.get<ZLinkSpotManager>(ZLINK_SPOT_MANAGER, { strict: false });
  for (let attempt = 0; state.zones().length < expectedLocalCapacity; attempt += 1) {
    const claimed = state.zones();
    const claimOrder: string[] = [];
    for (const zoneId of claimed) {
      for (const adjacent of adjacentZones(zoneId as ZoneId)) {
        if (candidates.includes(adjacent) && !claimed.includes(adjacent) && !claimOrder.includes(adjacent)) {
          claimOrder.push(adjacent);
        }
      }
    }
    claimOrder.push(...candidates.filter((zoneId) => !claimOrder.includes(zoneId)));
    for (const zoneId of claimOrder) {
      try {
        const result = await spots
          .getOrCreate(zoneId, ZoneSpot.name)
          .inMesh(ZoneWorldNames.zoneMesh)
          .submit();
        console.log(`zone spot create zone=${zoneId} state=${result.state}`);
      } catch (error) {
        if (!(error instanceof Error) || !/capacity|eligible User Spot placement target/i.test(error.message)) {
          throw error;
        }
      }
      if (!sameZones(state.zones(), claimed)) break;
    }
    if (state.zones().length >= expectedLocalCapacity) break;
    if (attempt + 1 >= zoneClaimRetryAttempts) {
      throw new Error(
        `Zone Spot capacity expected ${expectedLocalCapacity} local zones, observed ${state.zones().length}.`
      );
    }
    await delay(zoneClaimRetryDelayMs);
  }
  if (state.zones().length !== expectedLocalCapacity) {
    throw new Error(
      `Zone Spot capacity expected ${expectedLocalCapacity} local zones, observed ${state.zones().length}.`
    );
  }
}

async function waitForEmptyZoneSet(state: NodeRuntimeState, nodeId: string): Promise<void> {
  for (let attempt = 0; state.zones().length !== 2; attempt += 1) {
    if (state.zones().length === 0 && attempt >= allowEmptyZoneSetReadyAttempt) return;
    if (attempt + 1 >= zoneClaimRetryAttempts) {
      throw new Error(
        `Zone Spot capacity did not settle. node=${nodeId} zones=${state.zones().join(',')}.`
      );
    }
    await delay(zoneClaimRetryDelayMs);
  }
}

function sameZones(left: readonly string[], right: readonly string[]): boolean {
  return left.length === right.length && left.every((zoneId, index) => zoneId === right[index]);
}

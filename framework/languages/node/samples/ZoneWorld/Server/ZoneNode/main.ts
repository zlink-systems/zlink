import 'reflect-metadata';
import * as fs from 'node:fs';
import { setTimeout as delay } from 'node:timers/promises';
import { NestFactory } from '@nestjs/core';
import {
  ZLINK_ACTOR_CLIENT,
  ZLINK_ACTOR_MANAGER,
  ZLINK_LOCATION_RUNTIME_QUERY,
  ZLINK_ROUTE_CLIENT,
  ZLINK_ROUTE_MESH_RUNTIME,
  ZLINK_ROUTE_MESH_RUNTIME_OPTIONS,
  ZLINK_SPOT_MANAGER
} from '@zlink-systems/nestjs';
import type {
  ZLinkActorClient,
  ZLinkActorManager,
  ZLinkLocationRuntimeQuery,
  ZLinkRouteClient,
  ZLinkRouteMeshRuntime,
  ZLinkRouteMeshRuntimeOptions,
  ZLinkSpotManager
} from '@zlink-systems/framework';
import { readConfigPath, ZONEWORLD_CONFIG } from '../Configuration/configuration';
import type { ZoneWorldConfiguration } from '../Configuration/configuration';
import { closeRuntime, waitForShutdown } from '../runtime-support';
import { ZoneWorldNames, zonesOf } from '../../Shared/spec';
import { createZoneNodeModule } from './zone-node-module';
import { ZoneSpot } from './Infrastructure/ZLink/Spots/zone-spot';
import { EnterWorldReq, ReportNodeStatusMsg } from '../../Shared/contracts';
import { MaintenanceStore } from '../Configuration/maintenance-store';
import { NodeRuntimeState } from './Domain/node-runtime-state';
import { botRoutes } from './Domain/bot-patrol';

let statusTimer: NodeJS.Timeout | undefined;

async function bootstrap(): Promise<void> {
  const ZoneNodeModule = createZoneNodeModule(hasConfiguredZones());
  const app = await NestFactory.createApplicationContext(ZoneNodeModule, {
    logger: false,
    abortOnError: false
  });
  const config = app.get<ZoneWorldConfiguration>(ZONEWORLD_CONFIG);
  const node = config.zoneNode;
  if (node === undefined) throw new Error('ZoneNode configuration is required.');
  const zones = zonesOf(node.nodeId);
  if (zones.length > 0) {
    const state = app.get(NodeRuntimeState);
    const maintenance = app.get(MaintenanceStore);
    state.restore(await maintenance.readAll());
    console.log(`maintenance restored node=${node.nodeId} enabled=${state.ownMaintenance()}`);
    if (node.waitForPlacementPeer === true) {
      const routeMeshRuntime = app.get<ZLinkRouteMeshRuntime>(ZLINK_ROUTE_MESH_RUNTIME, { strict: false });
      await waitForPlacementPeer(routeMeshRuntime, ZoneWorldNames.zoneMesh);
    }
    const spots = app.get<ZLinkSpotManager>(ZLINK_SPOT_MANAGER, { strict: false });
    for (const zoneId of zones) {
      const result = await spots
        .getOrCreate(zoneId, ZoneSpot.name)
        .inMesh(ZoneWorldNames.zoneMesh)
        .submit();
      console.log(`zone spot create zone=${zoneId} state=${result.state}`);
      if (result.state === 'rejected') {
        throw new Error(`Zone spot '${zoneId}' creation was rejected.`);
      }
    }
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
    const channels = app.get<ZLinkRouteClient>(ZLINK_ROUTE_CLIENT, { strict: false });
    const report = async () => {
      try {
        await channels.sendToChannel(
          ZoneWorldNames.reportChannel,
          new ReportNodeStatusMsg(
            node.nodeId,
            [...zones],
            state.playerCount(),
            state.ownMaintenance()
          )
        ).submit();
        console.log(`node status submitted node=${node.nodeId}`);
      } catch {
        // Ops may start after this node; the periodic report retries through the public channel.
      }
    };
    statusTimer = setInterval(() => { void report(); }, 1_000);
    await report();
  }
  console.log(`topology=ready node=${node.nodeId} zones=${zones.join(',')}`);
  try {
    await waitForShutdown();
  } finally {
    if (statusTimer !== undefined) clearInterval(statusTimer);
    await closeRuntime(app);
  }
}

function hasConfiguredZones(): boolean {
  const configPath = readConfigPath(process.argv.slice(2));
  const document = JSON.parse(fs.readFileSync(configPath, 'utf8')) as {
    sample?: { zoneNode?: { nodeId?: unknown } };
  };
  const nodeId = document.sample?.zoneNode?.nodeId;
  return typeof nodeId === 'string' && zonesOf(nodeId).length > 0;
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
    ).timeout(10_000).submit<{ error: string | null }>();
    if (entered.error !== null) throw new Error(`Bot '${route.playerId}' could not enter the world: ${entered.error}.`);
    console.log(`bot spawned bot=${route.playerId} zone=${route.zoneId}`);
  }
}

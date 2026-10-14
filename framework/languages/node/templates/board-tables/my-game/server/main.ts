import 'reflect-metadata';
import { diagnostics } from './shared/diagnostics';
import '../.zlink-framework-json-schemas.cjs';
import { NestFactory } from '@nestjs/core';
import { Global, Module } from '@nestjs/common';
import { createInterface } from 'node:readline';
import {
  ZLinkRedisLocationStore,
  ZLinkRedisRelocationStore,
} from '@zlink-systems/framework-locations-redis';
import {
  zlinkFramework,
  zlinkModule,
  ZLinkModule,
  ZLINK_FRAMEWORK_RUNTIME,
  ZLINK_ROUTE_MESH_RUNTIME,
} from '@zlink-systems/nestjs';
import {
  ZLinkFrameworkRelocationMode,
  ZLinkFrameworkRelocationOutcome,
  ZLinkFrameworkTerminationOutcome,
  ZLinkFrameworkTerminationReason,
  ZLinkUserSpotExecutionMode,
  type ZLinkFrameworkRuntime,
  type ZLinkRouteMeshRuntime,
} from '@zlink-systems/framework';
import { Names, Limits, Control } from './shared/constants';
import { NODE_CONFIG, readConfig } from './shared/config';
import { SessionFactory } from './session/session';
import { Entry, Table, TableRelocation } from './table/infrastructure/table';
import { PlayerFactory, PlayerRelocation } from './table/infrastructure/player';
import { EntryJoin, Rejoin, View, Start, Bet, Deliver } from './table/infrastructure/handlers';
async function main(): Promise<void> {
  const config = readConfig(process.argv[2]);
  const session = config.streamEndpoint !== null;
  const location = new ZLinkRedisLocationStore({
    url: `redis://${config.redisEndpoint}`,
    keyPrefix: `${config.keyPrefix}location`,
  });
  const relocation = new ZLinkRedisRelocationStore({
    url: `redis://${config.redisEndpoint}`,
    keyPrefix: `${config.keyPrefix}relocation`,
  });
  @Global()
  @Module({ providers: [{ provide: NODE_CONFIG, useValue: config }], exports: [NODE_CONFIG] })
  class Configuration {}
  class Host {}
  const builder = zlinkFramework();
  builder.addLocationStore(location);
  builder.addRelocationStore(relocation);
  builder.configureDispatch().messageFlow('normal');
  const mesh = builder
    .addRouteMesh(Names.mesh)
    .setRoutingIdPrefix(config.id)
    .listen(config.meshEndpoint);
  if (session) {
    mesh.objects().client();
    builder
      .addStreamNode(Names.stream)
      .enableActorDispatch()
      .bind(config.streamEndpoint!)
      .registerSession(SessionFactory);
  } else {
    const objects = mesh.objects().server();
    objects.addEntrySpot(Entry);
    objects.addActorFactory(Names.actor, PlayerFactory, (f) =>
      f.preserveStateWith(PlayerRelocation),
    );
    objects.addSpotFactory(Names.table, Table, (f) => {
      f.executionMode(ZLinkUserSpotExecutionMode.SpotWide);
      f.preserveStateWith(TableRelocation);
    });
  }
  zlinkModule({
    providerDiscovery: [],
    imports: [Configuration, ZLinkModule.forRoot(builder.build())],
    providers: session
      ? [SessionFactory]
      : [
          Entry,
          Table,
          TableRelocation,
          PlayerFactory,
          PlayerRelocation,
          EntryJoin,
          Rejoin,
          View,
          Start,
          Bet,
          Deliver,
        ],
  })(Host);
  const app = await NestFactory.createApplicationContext(Host, {
    logger: ['error', 'warn'],
    abortOnError: false,
  });
  const runtime = app.get<ZLinkFrameworkRuntime>(ZLINK_FRAMEWORK_RUNTIME);
  const meshes = app.get<ZLinkRouteMeshRuntime>(ZLINK_ROUTE_MESH_RUNTIME);
  async function waitReady(): Promise<void> {
    for await (const observed of meshes.observe(
      Names.mesh,
      Limits.observationCapacity,
      AbortSignal.timeout(Limits.startupMs),
    )) {
      if (observed.loss.discardedTerminalCount !== 0n)
        throw new Error('Readiness observation lost');
      const status = observed.status;
      if (status.isReady && (session ? status.readyPeerCount > 0 : status.placement.isAvailable))
        return;
    }
    throw new Error('Readiness observation deadline expired');
  }
  async function waitSafe(): Promise<void> {
    for await (const observed of runtime.observe(AbortSignal.timeout(Limits.relocationMs))) {
      if (observed.loss.discardedTerminalCount !== 0n)
        throw new Error('Lifecycle observation lost');
      if (observed.status.safeToShutdown) return;
    }
    throw new Error('SafeToShutdown observation deadline expired');
  }
  const input = createInterface({ input: process.stdin });
  const failures: unknown[] = [];
  try {
    await waitReady();
    const listener = session
      ? runtime.getListenerStatus('stream', Names.stream)
      : runtime.getListenerStatus('routeMesh', Names.mesh);
    console.log(Control.ready + JSON.stringify({ id: config.id, endpoint: listener.endpoint }));
    for await (const line of input) {
      if (line === Control.drain) {
        if (session) throw new Error('Session drain is not a table operation');
        console.log(Control.requested);
        const result = await runtime.relocate({
          mode: ZLinkFrameworkRelocationMode.PlannedMaintenance,
          deadlineMs: Limits.relocationMs,
        });
        if (result.outcome !== ZLinkFrameworkRelocationOutcome.Relocated)
          throw new Error(`Relocation blocked: ${result.reason}`);
        console.log(Control.relocated);
        await waitSafe();
        console.log(Control.safe);
      } else if (line === Control.shutdown) break;
      else throw new Error(`Unknown control command: ${line}`);
    }
  } catch (error) {
    failures.push(error);
    console.error('Node operation failed', error);
  } finally {
    input.close();
    const cleanup = [
      async (): Promise<void> => {
        const result = await runtime.shutdown({ deadlineMs: Limits.shutdownMs });
        console.log(Control.stopped + JSON.stringify(result));
        if (
          result.outcome !== ZLinkFrameworkTerminationOutcome.Stopped ||
          result.reason !== ZLinkFrameworkTerminationReason.None
        )
          throw new Error(`Shutdown failed: ${result.outcome}/${result.reason}`);
      },
      (): Promise<void> => app.close(),
      (): Promise<void> => location.dispose(),
      (): Promise<void> => relocation.dispose(),
      (): Promise<void> => diagnostics.shutdown(),
    ];
    for (const release of cleanup) {
      try {
        await release();
      } catch (error) {
        failures.push(error);
        console.error('Node cleanup failed', error);
      }
    }
  }
  if (failures.length) throw new AggregateError(failures, 'Node operation or cleanup failed');
}
main().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});

import 'reflect-metadata';
import { diagnostics } from './diagnostics';
import '../../.zlink-framework-json-schemas.cjs';
import { NestFactory } from '@nestjs/core';
import { Global, Module, type Type, type INestApplicationContext } from '@nestjs/common';
import { createInterface } from 'node:readline';
import {
  ZLinkRedisLocationStore,
  ZLinkRedisRelocationStore
} from '@zlink-systems/framework-locations-redis';
import {
  zlinkFramework,
  zlinkModule,
  ZLinkModule,
  ZLINK_FRAMEWORK_RUNTIME,
  ZLINK_ROUTE_MESH_RUNTIME,
  type ZLinkNestMeshNodeBuilder
} from '@zlink-systems/nestjs';
import {
  ZLinkFrameworkRuntimeState,
  ZLinkFrameworkRelocationMode,
  ZLinkFrameworkRelocationOutcome,
  ZLinkFrameworkTerminationOutcome,
  ZLinkFrameworkTerminationReason,
  type ZLinkFrameworkRuntime,
  type ZLinkFrameworkRuntimeStatus,
  type ZLinkRouteMeshRuntime,
  type ZLinkRouteMeshStatus
} from '@zlink-systems/framework';
import { Names, Limits, Control } from './constants';
import { NODE_CONFIG, readConfig, type NodeConfig } from './config';
interface HostComposition {
  providers: Type[];
  listener: { kind: 'stream' | 'routeMesh'; name: string };
  ready: (status: ZLinkRouteMeshStatus) => boolean;
  endpoint?: () => string;
  release?: () => Promise<void>;
  initialize?: (app: INestApplicationContext) => Promise<void>;
}
export async function runHost(
  configure: (
    builder: ReturnType<typeof zlinkFramework>,
    config: NodeConfig,
    mesh: ZLinkNestMeshNodeBuilder
  ) => HostComposition
): Promise<void> {
  const config = readConfig(process.argv[2]);
  const location = new ZLinkRedisLocationStore({
    url: `redis://${config.redisEndpoint}`,
    keyPrefix: `${config.keyPrefix}location`
  });
  const relocation = new ZLinkRedisRelocationStore({
    url: `redis://${config.redisEndpoint}`,
    keyPrefix: `${config.keyPrefix}relocation`
  });
  @Global()
  @Module({
    providers: [{ provide: NODE_CONFIG, useValue: config }],
    exports: [NODE_CONFIG]
  })
  class Configuration {}
  class Host {}
  const builder = zlinkFramework();
  builder.configureLocations().messageFollowDurationMs(Limits.messageFollowMs);
  builder.addLocationStore(location);
  builder.addRelocationStore(relocation);
  builder.configureDispatch().messageFlow('normal');
  const mesh = builder
    .addRouteMesh(Names.mesh)
    .setRoutingIdPrefix(config.id)
    .listen(config.meshEndpoint);
  const composition = configure(builder, config, mesh);
  zlinkModule({
    providerDiscovery: [],
    imports: [Configuration, ZLinkModule.forRoot(builder.build())],
    providers: composition.providers
  })(Host);
  const app = await NestFactory.createApplicationContext(Host, {
    logger: ['error', 'warn'],
    abortOnError: false
  });
  const runtime = app.get<ZLinkFrameworkRuntime>(ZLINK_FRAMEWORK_RUNTIME);
  const meshes = app.get<ZLinkRouteMeshRuntime>(ZLINK_ROUTE_MESH_RUNTIME);
  function describe(observed: unknown): string {
    return JSON.stringify(observed, (_, value: unknown) =>
      typeof value === 'bigint' ? value.toString() : value
    );
  }
  async function waitServing(): Promise<void> {
    for await (const observed of runtime.observe(AbortSignal.timeout(Limits.startupMs))) {
      if (observed.loss.discardedTerminalCount !== 0n) throw new Error('Startup observation lost');
      if (observed.status.state === ZLinkFrameworkRuntimeState.Serving) return;
      if (observed.status.state !== ZLinkFrameworkRuntimeState.Preparing)
        throw new Error(`Runtime startup failed: ${observed.status.state}`);
    }
    throw new Error('Serving observation deadline expired');
  }
  async function waitReady(): Promise<void> {
    let lastStatus: ZLinkRouteMeshStatus | undefined;
    for await (const observed of meshes.observe(
      Names.mesh,
      Limits.observationCapacity,
      AbortSignal.timeout(Limits.startupMs)
    )) {
      if (observed.loss.discardedTerminalCount !== 0n)
        throw new Error('Readiness observation lost');
      const status = observed.status;
      lastStatus = status;
      if (status.isReady && composition.ready(status)) return;
    }
    throw new Error(
      'Readiness observation deadline expired: ' +
        (lastStatus
          ? describe({ status: lastStatus, compositionReady: composition.ready(lastStatus) })
          : 'no status observed')
    );
  }
  async function waitSafe(): Promise<void> {
    let lastStatus: ZLinkFrameworkRuntimeStatus | undefined;
    for await (const observed of runtime.observe(AbortSignal.timeout(Limits.safeShutdownMs))) {
      if (observed.loss.discardedTerminalCount !== 0n)
        throw new Error('Lifecycle observation lost');
      lastStatus = observed.status;
      if (lastStatus.safeToShutdown) return;
    }
    throw new Error(
      'SafeToShutdown observation deadline expired: ' +
        (lastStatus ? describe({ status: lastStatus }) : 'no status observed')
    );
  }
  const input = createInterface({ input: process.stdin });
  const failures: unknown[] = [];
  try {
    await waitServing();
    await waitReady();
    await composition.initialize?.(app);
    const listener = runtime.getListenerStatus(
      composition.listener.kind,
      composition.listener.name
    );
    console.log(
      Control.ready +
        JSON.stringify({
          id: config.id,
          endpoint: composition.endpoint?.() ?? listener.endpoint,
          httpEndpoint: composition.endpoint?.() ?? null,
          meshEndpoint: runtime.getListenerStatus('routeMesh', Names.mesh).endpoint,
          streamEndpoint: composition.listener.kind === 'stream' ? listener.endpoint : null
        })
    );
    for await (const line of input) {
      if (line === Control.drain) {
        console.log(Control.requested);
        const result = await runtime.relocate({
          mode: ZLinkFrameworkRelocationMode.PlannedMaintenance,
          deadlineMs: Limits.relocationMs
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
        await composition.release?.();
      },
      async (): Promise<void> => {
        const result = await runtime.shutdown({
          deadlineMs: Limits.shutdownMs
        });
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
      (): Promise<void> => diagnostics.shutdown()
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
export function failHost(error: unknown): void {
  console.error(error);
  process.exitCode = 1;
}

import 'reflect-metadata';
import * as http from 'node:http';
import * as path from 'node:path';
import { Module, Provider, Type } from '@nestjs/common';
import { INestApplicationContext } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import {
  ZLINK_CLIENT_SERVER_RUNTIME, ZLINK_FANOUT_RUNTIME, ZLINK_FRAMEWORK_RUNTIME, ZLINK_ROUTE_MESH_RUNTIME, ZLinkModule, zlinkFramework
} from '@zlink-systems/nestjs';
import type {
  ZLinkClientServerRuntime, ZLinkFanoutRuntime, ZLinkFrameworkRuntime, ZLinkRouteMeshRuntime, ZLinkWorkerOptions
} from '@zlink-systems/framework';
import { ZLinkRedisLocationStore } from '@zlink-systems/framework-locations-redis';
import { version as coreVersion } from '@zlink-systems/zlink';
import { PerfClock } from '../shared/clock';
import { PerfReady, PerfTriggerRequest, readJson, ResetRequest, RoleConfig, toJson, PerfValidationException } from '../shared/contracts';
import { Measurement } from '../shared/measurement';
import { enableFlowFileLogging } from './message-flow-file';
import { PublicMetricCollector } from './public-metric-collector';

export const ROLE_CONFIG = Symbol('perf.RoleConfig');
export type FrameworkBuilder = ReturnType<typeof zlinkFramework>;

// The role's own statement that its cell objects (Spot, Actor, subscriptions) are not yet prepared (§16.1 objectsReady).
// The role replaces the statement as its public create/bind results arrive; the evidence lists those results.
export class ObjectsReadiness {
  private state: { ready: boolean; reason: string; evidence: unknown[] };
  constructor(ready: boolean, reason: string) {
    this.state = { ready, reason, evidence: [] };
  }
  get ready(): boolean { return this.state.ready; }
  get reason(): string { return this.state.reason; }
  get evidence(): unknown[] { return this.state.evidence; }
  set(ready: boolean, reason: string, evidence: unknown[]): void { this.state = { ready, reason, evidence }; }
}

export function readConfig(argv: string[]): { config: RoleConfig; cellDirectory: string } {
  if (argv.length !== 2 || argv[0] !== '--config') throw new Error('Server requires --config <file> only.');
  const config = readJson<RoleConfig>(argv[1]);
  if (new URL(config.metricsUrl).port === new URL(config.applicationTriggerUrl).port) throw new Error('Admin and application trigger require separate listeners.');
  // role-configs/<role>.json sits one folder below the cell directory (perf §15.1), where the sequence originals are written.
  return { config, cellDirectory: path.dirname(path.dirname(path.resolve(argv[1]))) };
}

export interface RoleOptions {
  config: RoleConfig;
  // Registers this role's RouteMesh, Channel, Spot, Actor, Stream and fanout topology on the Framework builder.
  configureFramework: (builder: FrameworkBuilder) => void;
  providers: Provider[];
  worker?: ZLinkWorkerOptions;
  objects?: ObjectsReadiness;
  // The measured loop of a source role; read when the runner's trigger arrives.
  workload?: () => (() => Promise<void>) | undefined;
  // Runs after the Framework host started: the role's setup, create, bind and probe calls.
  prepare?: (app: INestApplicationContext) => Promise<void>;
}

interface Runtimes {
  host: ZLinkFrameworkRuntime;
  mesh: ZLinkRouteMeshRuntime;
  clientServer: ZLinkClientServerRuntime;
  fanout: ZLinkFanoutRuntime;
}

export async function runRole(options: RoleOptions, measurement: Measurement): Promise<void> {
  const { config } = options;
  const collector = new PublicMetricCollector();
  let runtimes: Runtimes | undefined;
  if (config.diagnostics) enableFlowFileLogging(config.diagnostics);

  const module = class RoleModule {};
  Module({
    imports: [ZLinkModule.forRootFactory({
      useFactory: () => {
        const builder = zlinkFramework();
        builder.options({ requestTimeoutMs: config.workload.requestTimeoutMs, metrics: { meterProvider: collector.meterProvider }, ...(options.worker ? { worker: options.worker } : {}) });
        builder.configureDispatch().messageFlow(config.diagnostics ? 'normal' : 'off');
        builder.configureNetwork().bindHost = '127.0.0.1';
        // Perf spec §20: the run-owned Docker Redis, one namespace per cell; only Store scenarios carry it.
        if (config.store) builder.addLocationStore(new ZLinkRedisLocationStore({ url: `redis://${config.store.endpoint}`, keyPrefix: `${config.store.namespace}:` }));
        options.configureFramework(builder);
        return builder.build();
      }
    })],
    providers: [{ provide: Measurement, useValue: measurement }, { provide: ROLE_CONFIG, useValue: config }, ...options.providers]
  })(module as Type);

  const admin = new AdminServer(config, measurement, options, collector, () => runtimes);
  await admin.listen();
  const app = await NestFactory.createApplicationContext(module, { logger: ['error', 'warn'] });
  runtimes = {
    host: app.get<ZLinkFrameworkRuntime>(ZLINK_FRAMEWORK_RUNTIME, { strict: false }),
    mesh: app.get<ZLinkRouteMeshRuntime>(ZLINK_ROUTE_MESH_RUNTIME, { strict: false }),
    clientServer: app.get<ZLinkClientServerRuntime>(ZLINK_CLIENT_SERVER_RUNTIME, { strict: false }),
    fanout: app.get<ZLinkFanoutRuntime>(ZLINK_FANOUT_RUNTIME, { strict: false })
  };
  measurement.samplePublicState = () => {
    const status = runtimes!.host.status;
    return { observedTicks: PerfClock.now().toString(), state: status.state, isReady: status.isReady, acceptingWork: status.acceptingWork,
      pressureState: status.capacity.applicationJobQueue.pressureState };
  };
  let closing = false;
  const close = (): void => {
    if (closing) return;
    closing = true;
    setTimeout(() => process.exit(0), 3000).unref();
    void app.close().finally(() => process.exit(0));
  };
  process.once('SIGTERM', close);
  process.once('SIGINT', close);
  if (options.prepare) {
    try { await options.prepare(app); } catch (error) { measurement.recordDiagnostic(error); }
  }
}

class AdminServer {
  private readonly servers: http.Server[] = [];
  constructor(
    private readonly config: RoleConfig, private readonly measurement: Measurement, private readonly options: RoleOptions,
    private readonly collector: PublicMetricCollector, private readonly runtimes: () => Runtimes | undefined
  ) {}

  async listen(): Promise<void> {
    const metricsPort = Number(new URL(this.config.metricsUrl).port);
    const triggerPort = Number(new URL(this.config.applicationTriggerUrl).port);
    for (const port of [metricsPort, triggerPort]) {
      const server = http.createServer((request, response) => void this.handle(port === metricsPort, request, response));
      // Admin traffic is few, large-body-free calls; never let keep-alive hold the process open on shutdown.
      server.keepAliveTimeout = 1000;
      await new Promise<void>((resolve, reject) => { server.once('error', reject); server.listen(port, '127.0.0.1', resolve); });
      this.servers.push(server);
    }
  }

  private async handle(isMetrics: boolean, request: http.IncomingMessage, response: http.ServerResponse): Promise<void> {
    const url = new URL(request.url ?? '/', 'http://127.0.0.1');
    const send = (status: number, value: unknown): void => {
      response.writeHead(status, { 'content-type': 'application/json' });
      response.end(toJson(value));
    };
    try {
      if (url.pathname.startsWith('/perf') !== isMetrics) return send404(response);
      if (request.method === 'GET' && url.pathname === '/perf/ready') return send(200, this.ready());
      if (request.method === 'GET' && url.pathname === '/perf/stats') {
        // The runner's final read seals phase-owned originals after the measurement window closes.
        this.measurement.finalSnapshot = url.searchParams.has('final');
        const snapshot = this.measurement.snapshot(this.publicStatus());
        snapshot.publicMetrics = await this.collector.snapshot();
        snapshot.provenance = { ...snapshot.provenance, coreVersion: coreVersion().join('.') };
        return send(200, snapshot);
      }
      if (request.method === 'POST' && url.pathname === '/perf/reset') {
        const reset = await readBody<ResetRequest>(request);
        if (typeof reset.runId !== 'string' || typeof reset.cellId !== 'string') throw new SyntaxError('Identity text fields must be non-null JSON strings.');
        const reply = this.measurement.resetPhase(reset, () => {
          this.runtimes()!.host.resetCapacityMetrics();
          return this.runtimes()!.host.status.capacity.measurementEpoch;
        });
        return send(reply.ok ? 200 : 409, reply);
      }
      if (request.method === 'POST' && url.pathname === '/app/perf/start') {
        const trigger = await readBody<PerfTriggerRequest>(request);
        if (typeof trigger.runId !== 'string' || typeof trigger.cellId !== 'string' || typeof trigger.phase !== 'string') throw new SyntaxError('Identity text fields must be non-null JSON strings.');
        const ready = this.ready();
        // §16.1: warmup starts after infrastructure and objects; only the measured barrier needs consumersReady (PS marker).
        if (!(trigger.phase === 'warmup' ? ready.infrastructureReady && ready.objectsReady : ready.ready)) return send(409, { reason: 'Readiness evidence is incomplete.', ready });
        const reply = this.measurement.startPhase(trigger, this.options.workload?.());
        return send(reply.accepted ? 200 : 409, reply);
      }
      return send404(response);
    } catch (error) {
      if (error instanceof SyntaxError || error instanceof PerfValidationException) return send(400, { reason: (error as Error).message });
      return send(500, { reason: String((error as Error).message ?? error) });
    }
  }

  private publicStatus(): unknown {
    const runtimes = this.runtimes();
    if (!runtimes) return null;
    const host = runtimes.host.status;
    const topology = this.config.topology;
    if (topology === 'routemesh') return { host, routeMesh: runtimes.mesh.snapshot(this.config.meshName!) };
    if (topology === 'clientserver') return { host, clientServer: runtimes.clientServer.snapshot(this.config.channelName!) };
    return { host };
  }

  private ready(): PerfReady {
    const { config, measurement } = this;
    const runtimes = this.runtimes();
    const reasons: string[] = [];
    let infrastructure = runtimes !== undefined && runtimes.host.status.isReady;
    const topology = config.topology;
    if (runtimes && topology === 'routemesh') {
      const mesh = runtimes.mesh.snapshot(config.meshName!);
      // Channel messaging §3: RouteMesh excludes the sending node itself from candidates. Only the source needs a selectable
      // remote target; the receiver proves dispatch by echo. A source that is itself the only Server of its return ChannelName
      // (send/send, §10.4) has no remote target by design; the coordinator states that in the role config (awaitRemoteTargets=false).
      infrastructure &&= mesh.isReady && (!config.source || !config.awaitRemoteTargets ||
        mesh.channels.some((channel) => channel.channelName === config.channelName && channel.isReady && channel.readyTargetCount > 0));
    } else if (runtimes && topology === 'clientserver') {
      const channel = runtimes.clientServer.snapshot(config.channelName!);
      infrastructure &&= channel.isReady && channel.readyTargetCount > 0;
    }
    if (runtimes && config.objectRole === 'ObjectClient' && config.meshName != null)
      infrastructure &&= runtimes.mesh.snapshot(config.meshName).readyPeerCount > 0;
    const probe = measurement.setupEvidence.length > 0;
    const objects = this.options.objects;
    const objectsReady = objects?.ready ?? true;
    const evidence: unknown[] = [{ kind: 'publicStatus', source: 'public Framework runtime status', observedValue: this.publicStatus() }];
    if (Object.keys(config.transportEndpoints).length > 0)
      evidence.push({ kind: 'verifiedListenerReservation', source: 'role config; coordinator OS bind reservation and public host startup', observedValue: config.transportEndpoints });
    if (objects) evidence.push(...objects.evidence);
    evidence.push(...Object.values(measurement.preparationEvidence));
    evidence.push(...measurement.setupEvidence, ...measurement.errorEvidence);
    if (!infrastructure) reasons.push('Public host/channel/listener infrastructure is not ready.');
    if (!objectsReady) reasons.push(objects!.reason);
    if (!probe) reasons.push('No successful typed probe echo has been observed.');
    if (measurement.hasErrors) reasons.push('Application preparation or phase failed.');
    return { runId: config.runId, cellId: config.cellId, role: config.role, roleInstance: config.roleInstance, infrastructureReady: infrastructure, objectsReady,
      consumersReady: probe, ready: infrastructure && objectsReady && probe && !measurement.hasErrors, observedAtUnixMs: PerfClock.unixMs(), evidence, reasons };
  }
}

function send404(response: http.ServerResponse): void {
  response.writeHead(404);
  response.end();
}

async function readBody<T>(request: http.IncomingMessage): Promise<T> {
  const chunks: Buffer[] = [];
  for await (const chunk of request) chunks.push(chunk as Buffer);
  const value: unknown = JSON.parse(Buffer.concat(chunks).toString('utf8'));
  if (typeof value !== 'object' || value === null || Array.isArray(value)) throw new SyntaxError('JSON body must be an object.');
  return value as T;
}

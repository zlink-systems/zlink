import 'reflect-metadata';
import fs from 'node:fs';
import http from 'node:http';
import { Injectable, Module, Scope } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import {
  ZLinkPacket,
  ZLinkPeerState,
  type ZLinkInstanceSpot,
  type ZLinkInstanceSpotContext,
  type ZLinkRouteMeshRuntime,
  type ZLinkSpotOutbound,
  type ZLinkSpotPacketHandler,
  type ZLinkSpotRequestHandler
} from '@zlink-systems/framework';
import {
  ZLinkRedisLocationStore,
  ZLinkRedisRelocationStore
} from '@zlink-systems/framework-locations-redis';
import {
  ZLINK_ROUTE_MESH_RUNTIME,
  ZLINK_SPOT_OUTBOUND,
  ZLinkModule,
  zlinkFramework
} from '@zlink-systems/nestjs';

const meshName = 'instance-spot.mesh';
const spotType = 'ScenarioInstanceSpot';

interface RoleOptions {
  readonly role: 'caller' | 'owner';
  readonly rid: string;
  readonly httpPort: number;
  readonly meshEndpoint: string;
  readonly peerRid?: string;
  readonly peerEndpoint?: string;
  readonly redisEndpoint: string;
  readonly redisKeyPrefix: string;
}

interface ProbeRequest {
  readonly spotId: string;
  readonly operationId: string;
  readonly action: string;
}

interface ProbeReply {
  readonly spotId: string;
  readonly operationId: string;
  readonly action: string;
  readonly instanceSpot: boolean;
}

interface OperationEvidence {
  entered: number;
  completed: number;
  instanceCount: number;
}

@ZLinkPacket('InstanceProbeReq')
class InstanceProbeReq implements ProbeRequest {
  constructor(
    readonly spotId: string,
    readonly operationId: string,
    readonly action: string
  ) {}
}

@ZLinkPacket('InstanceProbeMsg')
class InstanceProbeMsg implements ProbeRequest {
  constructor(
    readonly spotId: string,
    readonly operationId: string,
    readonly action: string
  ) {}
}

@Injectable({ scope: Scope.TRANSIENT })
class ScenarioInstanceSpot implements ZLinkInstanceSpot {
  readonly context!: ZLinkInstanceSpotContext;
  operations = 0;

  configure(): void {
    this.context.handlers.addPacket(InstanceProbeHandler);
    this.context.handlers.addPacket(InstanceProbeSendHandler);
  }

  async onInitialize(): Promise<void> {
    materializedInstances += 1;
  }

  async onClosing(): Promise<void> {
    closedInstances += 1;
  }
}

@Injectable()
@ZLinkPacket('InstanceProbeReq')
class InstanceProbeHandler implements ZLinkSpotRequestHandler<
  ScenarioInstanceSpot,
  InstanceProbeReq,
  ProbeReply
> {
  async handle(spot: ScenarioInstanceSpot, request: InstanceProbeReq): Promise<ProbeReply> {
    spot.operations += 1;
    record(request.operationId, 'entered');
    record(request.operationId, 'completed');
    return {
      spotId: request.spotId,
      operationId: request.operationId,
      action: request.action,
      instanceSpot: true
    };
  }
}

@Injectable()
@ZLinkPacket('InstanceProbeMsg')
class InstanceProbeSendHandler implements ZLinkSpotPacketHandler<ScenarioInstanceSpot, InstanceProbeMsg> {
  async handle(spot: ScenarioInstanceSpot, message: InstanceProbeMsg): Promise<void> {
    spot.operations += 1;
    record(message.operationId, 'entered');
    record(message.operationId, 'completed');
  }
}

let options: RoleOptions;
let app: Awaited<ReturnType<typeof NestFactory.createApplicationContext>>;
let stopping = false;
let materializedInstances = 0;
let closedInstances = 0;
const evidence = new Map<string, OperationEvidence>();

void main().catch((error: unknown) => {
  console.error(error instanceof Error ? error.stack : String(error));
  process.exitCode = 1;
});

async function main(): Promise<void> {
  options = readOptions();
  const builder = zlinkFramework()
    .addLocationStore(new ZLinkRedisLocationStore({
      url: options.redisEndpoint,
      keyPrefix: options.redisKeyPrefix
    }))
    .addRelocationStore(new ZLinkRedisRelocationStore({
      url: options.redisEndpoint,
      keyPrefix: `${options.redisKeyPrefix}:relocation`
    }));
  builder.configureLocations()
    .pollingIntervalMs(20)
    .ownerLeaseRenewIntervalMs(100)
    .ownerLeaseTtlMs(5_000)
    .ownerLeaseFencingMarginMs(500)
    .ownerLeaseRenewTimeoutMs(500);
  const mesh = builder.addRouteMesh(meshName)
    .listen(options.meshEndpoint)
    .routingId(options.rid);
  if (options.role === 'owner') {
    mesh.objects().server().addInstanceSpotFactory(
      spotType,
      ScenarioInstanceSpot,
      (factory) => factory.disableRelocation()
    );
  } else {
    mesh.objects().client();
    if (options.peerRid !== undefined && options.peerEndpoint !== undefined) {
      mesh.peerConnections().connect(options.peerRid, options.peerEndpoint);
    }
  }

  class RoleModule {}
  Module({
    imports: [ZLinkModule.forRoot(builder.build())],
    providers: options.role === 'owner'
      ? [ScenarioInstanceSpot, InstanceProbeHandler]
      : []
  })(RoleModule);
  app = await NestFactory.createApplicationContext(RoleModule, {
    logger: false,
    abortOnError: false
  });
  const runtime = app.get(ZLINK_ROUTE_MESH_RUNTIME, { strict: false }) as ZLinkRouteMeshRuntime | undefined;
  const outbound = options.role === 'caller'
    ? app.get(ZLINK_SPOT_OUTBOUND, { strict: false }) as ZLinkSpotOutbound
    : undefined;
  const server = http.createServer((request, response) => {
    void handleRequest(request, response, runtime, outbound);
  });
  await new Promise<void>((resolve) => server.listen(options.httpPort, '127.0.0.1', resolve));
  process.once('SIGINT', () => { stopping = true; });
  process.once('SIGTERM', () => { stopping = true; });
  while (!stopping) await delay(20);
  await closeServer(server);
  await app.close();
}

async function handleRequest(
  request: http.IncomingMessage,
  response: http.ServerResponse,
  runtime: ZLinkRouteMeshRuntime | undefined,
  outbound: ZLinkSpotOutbound | undefined
): Promise<void> {
  try {
    const url = new URL(request.url ?? '/', 'http://127.0.0.1');
    if (request.method === 'GET' && url.pathname === '/health') {
      return writeJson(response, 200, { ready: true, role: options.role });
    }
    if (request.method === 'GET' && url.pathname === '/ready') {
      const targetRid = url.searchParams.get('targetRid');
      const snapshot = runtime?.snapshot(meshName);
      const ready = snapshot?.peers.some((peer) =>
        String(peer.nodeRid) === targetRid && peer.state === ZLinkPeerState.Ready
      ) ?? false;
      return writeJson(response, ready ? 200 : 503, { ready, targetRid });
    }
    if (request.method === 'GET' && url.pathname === '/evidence') {
      const operationId = url.searchParams.get('operationId') ?? '';
      return writeJson(response, 200, evidence.get(operationId) ?? {
        entered: 0,
        completed: 0,
        instanceCount: materializedInstances
      });
    }
    if (request.method === 'GET' && url.pathname === '/lifecycle') {
      return writeJson(response, 200, { materializedInstances, closedInstances });
    }
    if (request.method === 'POST' && url.pathname === '/shutdown') {
      stopping = true;
      return writeJson(response, 200, { stopping: true });
    }
    if (request.method === 'POST' && url.pathname === '/instance/request') {
      if (outbound === undefined) return writeJson(response, 409, { error: 'caller role is required' });
      const body = await readJson(request) as ProbeRequest;
      const reply = await outbound
        .requestToSpot(body.spotId, new InstanceProbeReq(body.spotId, body.operationId, body.action))
        .instanceSpot(spotType)
        .inMesh(meshName)
        .timeout(5_000)
        .submit<ProbeReply>();
      return writeJson(response, 200, {
        status: 'completed',
        spotId: reply.spotId,
        operationId: reply.operationId,
        action: reply.action,
        instanceSpot: reply.instanceSpot
      });
    }
    if (request.method === 'POST' && url.pathname === '/instance/send') {
      if (outbound === undefined) return writeJson(response, 409, { error: 'caller role is required' });
      const body = await readJson(request) as ProbeRequest;
      await outbound
        .sendToSpot(body.spotId, new InstanceProbeMsg(body.spotId, body.operationId, body.action))
        .instanceSpot(spotType)
        .inMesh(meshName)
        .submit();
      return writeJson(response, 200, {
        status: 'accepted',
        spotId: body.spotId,
        operationId: body.operationId,
        action: body.action
      });
    }
    return writeJson(response, 404, { error: 'not found' });
  } catch (error) {
    return writeJson(response, 500, {
      error: error instanceof Error ? error.message : String(error),
      kind: typeof error === 'object' && error !== null && 'kind' in error
        ? error.kind
        : undefined
    });
  }
}

function record(operationId: string, field: keyof Pick<OperationEvidence, 'entered' | 'completed'>): void {
  const current = evidence.get(operationId) ?? {
    entered: 0,
    completed: 0,
    instanceCount: materializedInstances
  };
  current[field] += 1;
  current.instanceCount = materializedInstances;
  evidence.set(operationId, current);
}

function readOptions(): RoleOptions {
  const argument = process.argv.find((value) => value.startsWith('--config='));
  if (argument === undefined) throw new Error('InstanceSpot role requires --config=<path>.');
  return JSON.parse(fs.readFileSync(argument.slice('--config='.length), 'utf8')) as RoleOptions;
}

async function readJson(request: http.IncomingMessage): Promise<unknown> {
  const chunks: Buffer[] = [];
  for await (const chunk of request) chunks.push(Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk));
  return chunks.length === 0 ? {} : JSON.parse(Buffer.concat(chunks).toString('utf8'));
}

function writeJson(response: http.ServerResponse, status: number, value: unknown): void {
  const body = Buffer.from(JSON.stringify(value));
  response.writeHead(status, {
    'content-type': 'application/json',
    'content-length': body.length
  });
  response.end(body);
}

async function closeServer(server: http.Server): Promise<void> {
  await new Promise<void>((resolve, reject) => server.close((error) => error ? reject(error) : resolve()));
}

function delay(milliseconds: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

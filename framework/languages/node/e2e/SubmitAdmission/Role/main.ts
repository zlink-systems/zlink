import 'reflect-metadata';
import fs from 'node:fs';
import http from 'node:http';
import { Module } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import {
  ZLinkPacket,
  type ZLinkFanoutClient,
  type ZLinkRouteMeshRuntime,
  type ZLinkRouteClient,
  type ZLinkRouteSendContext,
  type ZLinkRouteSendHandler,
  type ZLinkSendContext,
  type ZLinkSendHandler
} from '@zlink-systems/framework';
import {
  ZLINK_FANOUT_CLIENT,
  ZLINK_ROUTE_CLIENT,
  ZLINK_ROUTE_MESH_RUNTIME,
  ZLinkModule,
  zlinkFramework
} from '@zlink-systems/nestjs';

const meshName = 'submit.mesh';
const channelName = 'submit.channel';
const fanoutName = 'submit.fanout';

interface RoleOptions {
  readonly role: 'caller' | 'target' | 'publisher';
  readonly rid: string;
  readonly httpPort: number;
  readonly meshEndpoint?: string;
  readonly peerRid?: string;
  readonly peerEndpoint?: string;
  readonly fanoutEndpoint?: string;
}

interface SubmitRequest {
  readonly operationId: string;
  readonly targetRid?: string;
}

interface OperationEvidence {
  entered: number;
  completed: number;
}

class AdmissionMessage {
  constructor(readonly operationId: string) {}
}

const evidence = new Map<string, OperationEvidence>();
let gateOpen = true;
let releaseGate: (() => void) | undefined;

function record(operationId: string, field: keyof OperationEvidence): void {
  const current = evidence.get(operationId) ?? { entered: 0, completed: 0 };
  current[field] += 1;
  evidence.set(operationId, current);
}

async function awaitGate(): Promise<void> {
  if (gateOpen) return;
  await new Promise<void>((resolve) => { releaseGate = resolve; });
}

@ZLinkPacket('AdmissionMessage')
class RouteAdmissionHandler implements ZLinkRouteSendHandler<AdmissionMessage> {
  async handle(message: AdmissionMessage, _context: ZLinkRouteSendContext): Promise<void> {
    record(message.operationId, 'entered');
    await awaitGate();
    record(message.operationId, 'completed');
  }
}

@ZLinkPacket('AdmissionMessage')
class ChannelAdmissionHandler implements ZLinkSendHandler<AdmissionMessage> {
  async handle(message: AdmissionMessage, _context: ZLinkSendContext): Promise<void> {
    record(message.operationId, 'entered');
    await awaitGate();
    record(message.operationId, 'completed');
  }
}

async function main(): Promise<void> {
  const options = readOptions();
  let stopping = false;

  class RoleModule {}
  Module({
    imports: [
      ZLinkModule.forRoot(zlinkOptions(options))
    ],
    providers: [RouteAdmissionHandler, ChannelAdmissionHandler]
  })(RoleModule);

  const app = await NestFactory.createApplicationContext(RoleModule, {
    logger: false,
    abortOnError: false
  });
  const route = app.get(ZLINK_ROUTE_CLIENT, { strict: false }) as ZLinkRouteClient;
  const fanout = app.get(ZLINK_FANOUT_CLIENT, { strict: false }) as ZLinkFanoutClient;
  const runtime = options.role === 'publisher'
    ? undefined
    : app.get(ZLINK_ROUTE_MESH_RUNTIME, { strict: false }) as ZLinkRouteMeshRuntime;

  const server = http.createServer(async (request, response) => {
    try {
      const body = await readJson(request);
      const result = await handleRequest(options, route, fanout, runtime, request, body, () => {
        stopping = true;
      });
      writeJson(response, result.status, result.body);
    } catch (error) {
      writeJson(response, 500, {
        error: error instanceof Error ? error.message : String(error)
      });
    }
  });
  await new Promise<void>((resolve) => server.listen(options.httpPort, '127.0.0.1', resolve));
  while (!stopping) await delay(20);
  await new Promise<void>((resolve, reject) => server.close((error) => error ? reject(error) : resolve()));
  await app.close();
}

function zlinkOptions(options: RoleOptions) {
  const builder = zlinkFramework();
  if (options.role === 'publisher') {
    builder.addFanoutChannel(fanoutName)
      .enablePublisher(requireValue(options.fanoutEndpoint, 'fanoutEndpoint'))
      .routingId(options.rid);
    return builder.build();
  }

  const mesh = builder.addRouteMesh(meshName)
    .listen(requireValue(options.meshEndpoint, 'meshEndpoint'))
    .routingId(options.rid)
    .addSendHandler('AdmissionMessage', RouteAdmissionHandler);
  mesh.channel(channelName).server()
    .setWeight(options.role === 'target' ? 100 : 0)
    .addSendHandler('AdmissionMessage', ChannelAdmissionHandler);
  if (options.peerEndpoint !== undefined && options.peerRid !== undefined) {
    mesh.peerConnections().connect(options.peerRid, options.peerEndpoint);
  }
  return builder.build();
}

async function handleRequest(
  options: RoleOptions,
  route: ZLinkRouteClient,
  fanout: ZLinkFanoutClient,
  runtime: ZLinkRouteMeshRuntime | undefined,
  request: http.IncomingMessage,
  body: unknown,
  stop: () => void
): Promise<{ status: number; body: unknown }> {
  const url = new URL(request.url ?? '/', 'http://127.0.0.1');
  if (request.method === 'GET' && url.pathname === '/health') {
    return { status: 200, body: { ready: true, role: options.role } };
  }
  if (request.method === 'GET' && url.pathname === '/ready') {
    const targetRid = url.searchParams.get('targetRid');
    const snapshot = runtime?.snapshot(meshName);
    const ready = snapshot?.peers.some((peer) => String(peer.rid) === targetRid && peer.ready) ?? false;
    return { status: ready ? 200 : 503, body: { ready, targetRid } };
  }
  if (request.method === 'GET' && url.pathname === '/evidence') {
    const operationId = url.searchParams.get('operationId') ?? '';
    return { status: 200, body: evidence.get(operationId) ?? { entered: 0, completed: 0 } };
  }
  if (request.method === 'POST' && url.pathname === '/gate/close') {
    gateOpen = false;
    return { status: 200, body: { gate: 'closed' } };
  }
  if (request.method === 'POST' && url.pathname === '/gate/open') {
    gateOpen = true;
    releaseGate?.();
    releaseGate = undefined;
    return { status: 200, body: { gate: 'open' } };
  }
  if (request.method === 'POST' && url.pathname === '/shutdown') {
    stop();
    return { status: 200, body: { stopping: true } };
  }

  const submit = body as SubmitRequest;
  if (request.method === 'POST' && url.pathname === '/submit/node') {
    await route
      .sendToNode(meshName, requireValue(submit.targetRid, 'targetRid'), new AdmissionMessage(submit.operationId))
      .submit();
    return { status: 200, body: terminal(submit.operationId, 'Submitted') };
  }
  if (request.method === 'POST' && url.pathname === '/submit/channel') {
    await route
      .sendToChannel(channelName, new AdmissionMessage(submit.operationId))
      .submit();
    return { status: 200, body: terminal(submit.operationId, 'Submitted') };
  }
  if (request.method === 'POST' && url.pathname === '/submit/fanout') {
    await fanout.publish(fanoutName, new AdmissionMessage(submit.operationId)).submit();
    return { status: 200, body: terminal(submit.operationId, 'Submitted') };
  }
  return { status: 404, body: { error: 'not found' } };
}

function terminal(operationId: string, status: string): unknown {
  return {
    operationId,
    status,
    publicInvocationCount: 1,
    terminalCount: 1
  };
}

function readOptions(): RoleOptions {
  const argument = process.argv.find((value) => value.startsWith('--config='));
  if (argument === undefined) throw new Error('SubmitAdmission role requires --config=<path>.');
  return JSON.parse(fs.readFileSync(argument.slice('--config='.length), 'utf8')) as RoleOptions;
}

function requireValue(value: string | undefined, name: string): string {
  if (value === undefined || value.length === 0) throw new Error(`${name} is required.`);
  return value;
}

async function readJson(request: http.IncomingMessage): Promise<unknown> {
  const parts: Buffer[] = [];
  for await (const part of request) parts.push(Buffer.isBuffer(part) ? part : Buffer.from(part));
  if (parts.length === 0) return {};
  return JSON.parse(Buffer.concat(parts).toString('utf8'));
}

function writeJson(response: http.ServerResponse, status: number, body: unknown): void {
  const encoded = Buffer.from(JSON.stringify(body));
  response.writeHead(status, { 'content-type': 'application/json', 'content-length': encoded.length });
  response.end(encoded);
}

function delay(milliseconds: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

main().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});

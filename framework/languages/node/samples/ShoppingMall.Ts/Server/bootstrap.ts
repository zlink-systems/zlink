import 'reflect-metadata';
import http from 'node:http';
import { URL } from 'node:url';
import { NestFactory } from '@nestjs/core';
import {
  ZLinkFrameworkRelocationMode,
  ZLinkFrameworkRelocationOutcome,
  type ZLinkFrameworkRuntime,
  type ZLinkLocationRuntimeQuery,
  type ZLinkRouteMeshRuntime
} from '@zlink-systems/framework';
import {
  ZLINK_FRAMEWORK_RUNTIME,
  ZLINK_LOCATION_RUNTIME_QUERY,
  ZLINK_ROUTE_MESH_RUNTIME
} from '@zlink-systems/nestjs';
import { SHOPPINGMALL_SAMPLE_CONFIG } from './Configuration/sample-config';
import { createCommerceApiServer } from './CommerceApi/commerce-api-server';
import { createShoppingMallCommerceApiModule } from './CommerceApi/commerce-api-module';
import { OrderWorkflowRouterPort } from './CommerceApi/Application/order-workflow-router-port';
import { StartOrderUseCase } from './CommerceApi/Application/start-order-use-case';
import { createShoppingMallWorkflowModule } from './OrderWorkflow/shoppingmall-workflow-module';
import { OrderStore } from './Shared/Store/order-store';
import { SampleNames } from '../Shared/Configuration/sample-names';
import type { ShoppingMallServerConfig } from './Configuration/sample-config';

type ShoppingMallRole = 'api-a' | 'api-b' | 'workflow-a' | 'workflow-b';

async function bootstrapShoppingMall(role: ShoppingMallRole): Promise<void> {
  const workflow = isWorkflowRole(role);
  const moduleType = workflow
    ? createShoppingMallWorkflowModule(role)
    : createShoppingMallCommerceApiModule(role);
  const app = await NestFactory.createApplicationContext(moduleType, {
    logger: false,
    abortOnError: false
  });
  const config = app.get<ShoppingMallServerConfig>(SHOPPINGMALL_SAMPLE_CONFIG);
  const endpoint = endpointForRole(role, config);
  const routeMeshRuntime = app.get<ZLinkRouteMeshRuntime>(ZLINK_ROUTE_MESH_RUNTIME, { strict: false });
  if (!workflow) {
    await waitForObjectRouteReadiness(routeMeshRuntime);
    process.stdout.write(`shoppingmall-ready kind=object-route node=${role} target=${SampleNames.workflowA}\n`);
    process.stdout.write(`shoppingmall-ready kind=object-route node=${role} target=${SampleNames.workflowB}\n`);
  }
  const listenUrl = new URL(endpoint);
  const server = workflow
    ? createHealthServer(role, endpoint, {
        locations: app.get(ZLINK_LOCATION_RUNTIME_QUERY, { strict: false }),
        routeMeshRuntime,
        frameworkRuntime: app.get<ZLinkFrameworkRuntime>(ZLINK_FRAMEWORK_RUNTIME, { strict: false })
      })
    : createCommerceApiServer(
      endpoint,
      role,
      app.get(OrderStore, { strict: false }),
      app.get(StartOrderUseCase, { strict: false }),
      app.get(OrderWorkflowRouterPort, { strict: false }),
      routeMeshRuntime
    );

  await new Promise<void>((resolve, reject) => {
    server.once('error', reject);
    server.listen(Number(listenUrl.port), listenUrl.hostname, () => {
      server.off('error', reject);
      console.log(`shoppingmall ${role} listening ${endpoint}`);
      if (!workflow) process.stdout.write(`shoppingmall-ready kind=http node=${role}\n`);
      resolve();
    });
  });
  await waitForShutdown();
  await new Promise<void>((resolve) => server.close(() => resolve()));
  await app.close();
}

function createHealthServer(roleName: string, baseEndpoint: string, dependencies: {
  locations: ZLinkLocationRuntimeQuery;
  routeMeshRuntime: ZLinkRouteMeshRuntime;
  frameworkRuntime: ZLinkFrameworkRuntime;
}): http.Server {
  return http.createServer(async (request, response) => {
    const url = new URL(request.url ?? '/', baseEndpoint);
    if (request.method === 'GET' && url.pathname === '/health') {
      try {
        const status = await dependencies.locations.getStatus();
        const mesh = dependencies.routeMeshRuntime.snapshot(SampleNames.orderWorkflowSpotMesh);
        const ready = status.storeHealthy && mesh.isReady && mesh.placement.isAvailable;
        response.writeHead(ready ? 200 : 503, { 'content-type': 'application/json' });
        response.end(JSON.stringify({
          ok: ready,
          role: roleName,
          routeMesh: {
            isReady: mesh.isReady,
            placementAvailable: mesh.placement.isAvailable,
            readyPeerCount: mesh.readyPeerCount
          }
        }));
      } catch (error) {
        console.error(`shoppingmall ${roleName} health probe failed: ${error instanceof Error ? error.message : String(error)}`);
        response.writeHead(503, { 'content-type': 'application/json' });
        response.end(JSON.stringify({ ok: false, role: roleName, error: error instanceof Error ? error.message : String(error) }));
      }
      return;
    }
    if (request.method === 'POST' && url.pathname === '/self-check/relocate') {
      try {
        const result = await dependencies.frameworkRuntime.relocate({
          mode: ZLinkFrameworkRelocationMode.PlannedMaintenance,
          deadlineMs: 30_000
        });
        sendJson(response, result.outcome === ZLinkFrameworkRelocationOutcome.Relocated ? 200 : 409, {
          outcome: result.outcome === ZLinkFrameworkRelocationOutcome.Relocated ? 'relocated' : 'blocked',
          reason: result.reason
        });
      } catch (error) {
        sendJson(response, 500, { error: error instanceof Error ? error.message : String(error) });
      }
      return;
    }
    response.writeHead(404, { 'content-type': 'application/json' });
    response.end(JSON.stringify({ error: `No route for ${request.method ?? 'GET'} ${url.pathname}` }));
  });
}

async function waitForObjectRouteReadiness(runtime: ZLinkRouteMeshRuntime): Promise<void> {
  for (let attempt = 0; attempt < 300; attempt += 1) {
    const mesh = runtime.snapshot(SampleNames.orderWorkflowSpotMesh);
    if (mesh.isReady && mesh.readyPeerCount >= 2) return;
    await new Promise<void>((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`ShoppingMall RouteMesh '${SampleNames.orderWorkflowSpotMesh}' did not discover both Workflow nodes.`);
}

function endpointForRole(role: ShoppingMallRole, config: ShoppingMallServerConfig): string {
  if (role === SampleNames.apiA) return config.apiAHttpUrl;
  if (role === SampleNames.apiB) return config.apiBHttpUrl;
  if (role === SampleNames.workflowA) return config.workflowAHttpUrl;
  return config.workflowBHttpUrl;
}

function isWorkflowRole(role: ShoppingMallRole): boolean {
  return role === SampleNames.workflowA || role === SampleNames.workflowB;
}

function waitForShutdown(): Promise<void> {
  return new Promise((resolve) => {
    const keepAlive = setInterval(() => undefined, 60_000);
    const stop = (): void => {
      clearInterval(keepAlive);
      resolve();
    };
    process.once('SIGINT', stop);
    process.once('SIGTERM', stop);
  });
}

function sendJson(response: http.ServerResponse, status: number, body: unknown): void {
  response.writeHead(status, { 'content-type': 'application/json' });
  response.end(JSON.stringify(body));
}

export { bootstrapShoppingMall };

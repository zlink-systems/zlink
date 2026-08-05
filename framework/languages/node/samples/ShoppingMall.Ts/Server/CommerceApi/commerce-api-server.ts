import http from 'node:http';
import { URL } from 'node:url';
import type {
  StartOrderReq
} from '../../Shared/Contracts/messages';
import type { ZLinkRouteMeshRuntime } from '@zlink-systems/framework';
import { SampleNames } from '../../Shared/Configuration/sample-names';
import { OrderStore } from '../Shared/Store/order-store';
import { OrderWorkflowRouterPort } from './Application/order-workflow-router-port';
import { StartOrderUseCase } from './Application/start-order-use-case';

function createCommerceApiServer(
  endpoint: string,
  role: string,
  store: OrderStore,
  startOrder: StartOrderUseCase,
  workflowRouter: OrderWorkflowRouterPort,
  routeMeshRuntime: ZLinkRouteMeshRuntime
): http.Server {
  return http.createServer(async (request, response) => {
    try {
      const url = new URL(request.url ?? '/', endpoint);
      if (request.method === 'GET' && url.pathname === '/health') {
        const mesh = routeMeshRuntime.snapshot(SampleNames.orderWorkflowSpotMesh);
        const ready = mesh.isReady && mesh.readyPeerCount >= 2;
        sendJson(response, ready ? 200 : 503, {
          ok: ready,
          role,
          routeMesh: {
            isReady: mesh.isReady,
            readyPeerCount: mesh.readyPeerCount
          }
        });
        return;
      }
      if (request.method === 'POST' && url.pathname === '/orders/start') {
        const body = await readJson(request) as StartOrderReq;
        sendJson(response, 200, await startOrder.start(body));
        return;
      }
      if (request.method === 'POST' && url.pathname === '/self-check/seed') {
        sendJson(response, 200, store.seedSelfCheck());
        return;
      }
      if (request.method === 'GET' && url.pathname.startsWith('/orders/')) {
        sendJson(response, 200, store.getOrder(decodeURIComponent(url.pathname.substring('/orders/'.length))));
        return;
      }
      if (request.method === 'POST' && url.pathname === '/self-check/idempotency/pending') {
        const body = await readJson(request);
        sendJson(response, 200, store.createPendingMapping(body.idempotencyKey, body.orderId, body.ownerInstanceId));
        return;
      }
      if (request.method === 'POST' && url.pathname === '/self-check/workflow/inventory-reserved') {
        const body = await readJson(request) as StartOrderReq;
        const result = await workflowRouter.prepareInventory(store.reserveOrder(body));
        sendJson(response, 200, { state: result.state });
        return;
      }
      if (request.method === 'POST' && url.pathname === '/self-check/workflow/inventory-effect') {
        const body = await readJson(request) as StartOrderReq;
        const result = await workflowRouter.prepareInventoryEffect(store.reserveOrder(body));
        sendJson(response, 200, { state: result.state });
        return;
      }
      const continueMatch = url.pathname.match(/^\/self-check\/workflow\/([^/]+)\/continue$/);
      if (request.method === 'POST' && continueMatch !== null) {
        const orderId = decodeURIComponent(continueMatch[1]);
        sendJson(response, 200, await workflowRouter.continue(orderId));
        return;
      }
      const deleteMatch = url.pathname.match(/^\/self-check\/projection\/([^/]+)\/delete$/);
      if (request.method === 'POST' && deleteMatch !== null) {
        sendJson(response, 200, store.deleteProjection(decodeURIComponent(deleteMatch[1])));
        return;
      }
      const rebuildMatch = url.pathname.match(/^\/self-check\/projection\/([^/]+)\/rebuild$/);
      if (request.method === 'POST' && rebuildMatch !== null) {
        const orderId = decodeURIComponent(rebuildMatch[1]);
        sendJson(response, 200, await workflowRouter.rebuild(orderId));
        return;
      }
      const fenceMatch = url.pathname.match(/^\/self-check\/workflow\/([^/]+)\/verify-fence$/);
      if (request.method === 'POST' && fenceMatch !== null) {
        sendJson(response, 200, await workflowRouter.verifyExpectedVersionFence(decodeURIComponent(fenceMatch[1])));
        return;
      }
      if (request.method === 'POST' && url.pathname === '/self-check/assert') {
        sendJson(response, 200, store.assertEvidence(await readJson(request)));
        return;
      }
      sendJson(response, 404, { error: `No route for ${request.method ?? 'GET'} ${url.pathname}` });
    } catch (error) {
      console.error(error);
      sendJson(response, 500, { error: error instanceof Error ? error.message : String(error) });
    }
  });
}

function readJson(request: http.IncomingMessage): Promise<any> {
  return new Promise((resolve, reject) => {
    const chunks: Buffer[] = [];
    request.on('data', (chunk: Buffer) => chunks.push(chunk));
    request.on('end', () => {
      const text = Buffer.concat(chunks).toString('utf8');
      resolve(text.length === 0 ? {} : JSON.parse(text));
    });
    request.on('error', reject);
  });
}

function sendJson(response: http.ServerResponse, status: number, body: unknown): void {
  response.writeHead(status, { 'content-type': 'application/json' });
  response.end(JSON.stringify(body));
}

export { createCommerceApiServer, sendJson };

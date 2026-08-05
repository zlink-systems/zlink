import http from 'node:http';
import { ZLINK_CHANNEL_CLIENT } from '@zlink-systems/nestjs';
import { assignDelivery } from '../../Shared/Contracts/messages';
import { SampleNames } from '../../Shared/Configuration/sample-names';
import type { INestApplicationContext } from '@nestjs/common';
import type { ZLinkChannelClient } from '@zlink-systems/framework';
import type {
  CreateDeliveryReq,
  CreateDeliveryRes,
  ServerAssertionReq,
  ServerAssertionRes
} from '../../Shared/Contracts/messages';
import type { EvidenceStore } from '../Configuration/evidence-store';
import type { DeliveryDispatchServerConfig } from '../Configuration/sample-config';

function startDispatchApi(
  app: INestApplicationContext,
  config: DeliveryDispatchServerConfig,
  evidence: EvidenceStore
): Promise<http.Server> {
  const channels = app.get(ZLINK_CHANNEL_CLIENT, { strict: false }) as ZLinkChannelClient;
  const server = http.createServer(async (request, response) => {
    try {
      if (request.method === 'GET' && request.url === '/health') {
        sendJson(response, 200, { ready: true, role: 'dispatch' });
        return;
      }
      if (request.method === 'POST' && request.url === '/deliveries') {
        const body = await readJson<CreateDeliveryReq>(request);
        submitDispatch(channels, body);
        console.error(`deliverydispatch api: created delivery=${body.deliveryId}`);
        sendJson(response, 200, { deliveryId: body.deliveryId } satisfies CreateDeliveryRes);
        return;
      }
      if (request.method === 'POST' && request.url === '/self-check/assert') {
        const body = await readJson<ServerAssertionReq>(request);
        const success = evidence.hasExactSequence(body.successfulDeliveryId, [
          { status: 'Assigned', courierId: 'courier-a' },
          { status: 'Accepted', courierId: 'courier-a' },
          { status: 'PickedUp', courierId: 'courier-a' },
          { status: 'Delivered', courierId: 'courier-a' }
        ]);
        const reassigned = evidence.hasExactSequence(body.reassignedDeliveryId, [
          { status: 'Assigned', courierId: 'courier-a' },
          { status: 'Reassigned', courierId: 'courier-b' },
          { status: 'Accepted', courierId: 'courier-b' },
          { status: 'PickedUp', courierId: 'courier-b' },
          { status: 'Delivered', courierId: 'courier-b' }
        ]);
        sendJson(response, 200, { passed: success && reassigned, evidence: evidence.readLines() } satisfies ServerAssertionRes);
        return;
      }
      sendJson(response, 404, { error: 'not-found' });
    } catch (error) {
      sendJson(response, 500, { error: error instanceof Error ? error.message : String(error) });
    }
  });

  const url = new URL(config.dispatchApiHttpUrl);
  return new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(Number(url.port), url.hostname, () => {
      server.off('error', reject);
      resolve(server);
    });
  });
}

function submitDispatch(
  channels: ZLinkChannelClient,
  request: CreateDeliveryReq
): void {
  channels
    .sendToChannel(
      SampleNames.dispatchChannel,
      assignDelivery(request.deliveryId, request.customerId, request.pickupAddress, request.dropoffAddress))
    .submit();
}

function readJson<T>(request: http.IncomingMessage): Promise<T> {
  return new Promise((resolve, reject) => {
    const chunks: Buffer[] = [];
    request.on('data', (chunk) => chunks.push(Buffer.from(chunk)));
    request.on('error', reject);
    request.on('end', () => {
      try {
        resolve(JSON.parse(Buffer.concat(chunks).toString('utf8')) as T);
      } catch (error) {
        reject(error);
      }
    });
  });
}

function sendJson(response: http.ServerResponse, statusCode: number, body: unknown): void {
  response.writeHead(statusCode, { 'content-type': 'application/json' });
  response.end(JSON.stringify(body));
}

export { startDispatchApi };

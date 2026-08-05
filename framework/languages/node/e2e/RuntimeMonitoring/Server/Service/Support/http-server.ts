import * as http from 'node:http';
import type { IncomingMessage, Server, ServerResponse } from 'node:http';

export interface HttpRoute {
  readonly method: string;
  readonly path: string;
  readonly handle: (body: unknown) => Promise<unknown> | unknown;
}

export async function startHttpServer(endpoint: string, routes: readonly HttpRoute[]): Promise<Server> {
  const server = http.createServer(async (request: IncomingMessage, response: ServerResponse) => {
    const route = routes.find((candidate) => candidate.method === request.method && candidate.path === request.url);
    if (route === undefined) {
      response.writeHead(404).end();
      return;
    }

    try {
      const body = request.method === 'GET' ? undefined : await readJson(request);
      const payload = JSON.stringify(await route.handle(body));
      response.writeHead(200, { 'content-type': 'application/json', connection: 'close' });
      response.end(payload);
    } catch (error) {
      response.writeHead(500, { 'content-type': 'application/json', connection: 'close' });
      response.end(JSON.stringify({ error: error instanceof Error ? error.message : String(error) }));
    }
  });
  const url = new URL(endpoint);
  await new Promise<void>((resolve, reject) => {
    server.once('error', reject);
    server.listen(Number(url.port), url.hostname, resolve);
  });
  return server;
}

export function closeHttpServer(server: Server): Promise<void> {
  return new Promise<void>((resolve, reject) => server.close((error) => error === undefined ? resolve() : reject(error)));
}

async function readJson(request: IncomingMessage): Promise<unknown> {
  const chunks: Buffer[] = [];
  for await (const chunk of request) {
    chunks.push(Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk));
  }
  if (chunks.length === 0) {
    return undefined;
  }
  return JSON.parse(Buffer.concat(chunks).toString('utf8'));
}

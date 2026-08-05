import http from 'node:http';

export interface HttpRoute {
  readonly method: string;
  readonly path: string;
  readonly handle: (body: unknown) => unknown | Promise<unknown>;
}

export async function startHttpServer(urlText: string, routes: readonly HttpRoute[]): Promise<http.Server> {
  const url = new URL(urlText);
  const server = http.createServer(async (request, response) => {
    const route = routes.find((candidate) => candidate.method === request.method && candidate.path === urlPath(request.url));
    if (route === undefined) {
      response.writeHead(404).end();
      return;
    }
    try {
      const body = await readJson(request);
      const result = await route.handle(body);
      const payload = JSON.stringify(result ?? {}, jsonReplacer);
      response.writeHead(200, { 'content-type': 'application/json' });
      response.end(payload);
    } catch (error) {
      console.error(`HTTP route failed ${request.method} ${request.url}`, error);
      response.writeHead(500, { 'content-type': 'application/json' });
      response.end(JSON.stringify({ error: error instanceof Error ? error.message : String(error) }));
    }
  });
  await new Promise<void>((resolve) => server.listen(Number(url.port), url.hostname, resolve));
  return server;
}

function jsonReplacer(_key: string, value: unknown): unknown {
  return typeof value === 'bigint' ? value.toString() : value;
}

export async function closeHttpServer(server: http.Server): Promise<void> {
  await new Promise<void>((resolve) => server.close(() => resolve()));
}

async function readJson(request: http.IncomingMessage): Promise<unknown> {
  if (request.method === 'GET') {
    return {};
  }
  const chunks: Buffer[] = [];
  for await (const chunk of request) {
    chunks.push(Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk));
  }
  const text = Buffer.concat(chunks).toString('utf8');
  return text.length === 0 ? {} : JSON.parse(text);
}

function urlPath(value: string | undefined): string {
  return new URL(value ?? '/', 'http://127.0.0.1').pathname;
}

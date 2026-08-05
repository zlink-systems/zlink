import http from 'node:http';
import { URL } from 'node:url';

export interface HttpRoute {
  readonly method: string;
  readonly path: string | RegExp;
  readonly handle: (body: unknown, match: RegExpMatchArray | undefined) => Promise<unknown> | unknown;
}

export async function startHttpServer(url: string, routes: readonly HttpRoute[]): Promise<http.Server> {
  const parsed = new URL(url);
  const server = http.createServer(async (request, response) => {
    const pathname = request.url?.split('?')[0] ?? '';
    const found = routes.map((route) => ({
      route,
      match: typeof route.path === 'string'
        ? (route.path === pathname ? [] as unknown as RegExpMatchArray : undefined)
        : pathname.match(route.path) ?? undefined
    })).find((candidate) => candidate.route.method === request.method && candidate.match !== undefined);
    if (found === undefined) {
      response.writeHead(404, { 'content-type': 'application/json' });
      response.end(JSON.stringify({ error: 'not found' }));
      return;
    }
    try {
      const result = await found.route.handle(await readJson(request), found.match);
      response.writeHead(200, { 'content-type': 'application/json' });
      response.end(JSON.stringify(result ?? {}));
    } catch (error) {
      console.error(error);
      response.writeHead(500, { 'content-type': 'application/json' });
      response.end(JSON.stringify({ error: error instanceof Error ? error.message : String(error) }));
    }
  });
  await new Promise<void>((resolve, reject) => {
    server.once('error', reject);
    server.listen(Number(parsed.port), parsed.hostname, () => {
      server.off('error', reject);
      resolve();
    });
  });
  return server;
}

export async function closeHttpServer(server: http.Server): Promise<void> {
  await new Promise<void>((resolve) => server.close(() => resolve()));
}

async function readJson(request: http.IncomingMessage): Promise<unknown> {
  if (request.method === 'GET') return undefined;
  const chunks: Buffer[] = [];
  for await (const chunk of request) chunks.push(Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk));
  const text = Buffer.concat(chunks).toString('utf8').trim();
  return text.length === 0 ? undefined : JSON.parse(text);
}

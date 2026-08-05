import http from 'node:http';
import { readExternalApiOptions } from './Configuration/external-api-options';

const config = readExternalApiOptions(process.argv.slice(2));
const endpoint = new URL(config.httpUrl);
const server = http.createServer((request, response) => {
  const url = new URL(request.url ?? '/', config.httpUrl);
  if (request.method === 'GET' && url.pathname === '/health') {
    reply(response, 200, { status: 'ready', role: 'external-api' });
    return;
  }
  if (request.method === 'GET' && url.pathname === '/delay') {
    const delayMs = boundedDelay(url.searchParams.get('delayMs'));
    const requestId = requiredQuery(url, 'requestId');
    const marker = requiredQuery(url, 'marker');
    setTimeout(() => reply(response, 200, { requestId, marker }), delayMs);
    return;
  }
  reply(response, 404, { error: 'not-found' });
});

server.listen(Number(endpoint.port), endpoint.hostname);
for (const signal of ['SIGINT', 'SIGTERM'] as const) {
  process.once(signal, () => server.close(() => process.exit(0)));
}

function requiredQuery(url: URL, name: string): string {
  const value = url.searchParams.get(name);
  if (value === null || value.length === 0) throw new Error(`Query '${name}' is required.`);
  return value;
}

function boundedDelay(value: string | null): number {
  const delayMs = Number(value);
  if (!Number.isInteger(delayMs) || delayMs < 0 || delayMs > 30_000) {
    throw new Error("Query 'delayMs' must be an integer from 0 through 30000.");
  }
  return delayMs;
}

function reply(response: http.ServerResponse, status: number, body: unknown): void {
  response.writeHead(status, { 'content-type': 'application/json' });
  response.end(JSON.stringify(body));
}

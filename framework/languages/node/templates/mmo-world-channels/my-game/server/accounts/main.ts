import { createServer } from 'node:http';
import { createInterface } from 'node:readline';
import { once } from 'node:events';
import { readConfig } from '../shared/config';
import { Control, Names } from '../shared/constants';
async function main(): Promise<void> {
  const config = readConfig(process.argv[2]);
  if (!config.httpEndpoint || !config.worlds?.length)
    throw new Error('Accounts requires HTTP endpoint and worlds');
  const endpoint = new URL(config.httpEndpoint);
  const worlds = config.worlds;
  const server = createServer((request, response) => {
    if (request.method !== 'GET' || request.url !== Names.worldsPath) {
      response.writeHead(404).end();
      return;
    }
    response.writeHead(200, { 'content-type': 'application/json' }).end(JSON.stringify(worlds));
  });
  server.listen(Number(endpoint.port), endpoint.hostname);
  await once(server, 'listening');
  const address = server.address();
  if (!address || typeof address === 'string') throw new Error('Missing HTTP address');
  console.log(
    Control.ready +
      JSON.stringify({ id: config.id, endpoint: `http://${endpoint.hostname}:${address.port}` })
  );
  const input = createInterface({ input: process.stdin });
  try {
    for await (const line of input) {
      if (line === Control.shutdown) break;
      throw new Error(`Accounts does not host Framework relocation: ${line}`);
    }
  } finally {
    input.close();
    await new Promise<void>((resolve, reject) =>
      server.close((error) => (error ? reject(error) : resolve()))
    );
    console.log(Control.stopped + JSON.stringify({ outcome: 'Stopped', reason: 'None' }));
  }
}
main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});

#!/usr/bin/env node
import net from 'node:net';
import http from 'node:http';
import { pathToFileURL } from 'node:url';
import { args, integer, main, rpc, writeJson } from './common.mjs';
import { LIMITS } from './constants.mjs';

// Pausing the readable side applies TCP backpressure instead of retaining an unbounded byte queue.
export async function startProxy({ listenPort = 0, controlPort = 0, target, stateFile }) {
  const destination = new URL(target);
  if (destination.protocol !== 'tcp:' && destination.protocol !== 'http:') throw new Error('target must use tcp: or http:');
  const pairs = new Set();
  let held = { upstream: false, downstream: false };
  const server = net.createServer({ highWaterMark: LIMITS.proxyBufferBytes }, (client) => {
    const remote = net.connect({ host: destination.hostname, port: Number(destination.port), highWaterMark: LIMITS.proxyBufferBytes });
    const pair = { client, remote };
    pairs.add(pair);
    client.pipe(remote); remote.pipe(client);
    if (held.upstream) client.pause();
    if (held.downstream) remote.pause();
    const close = () => { pairs.delete(pair); client.destroy(); remote.destroy(); };
    client.on('error', close); remote.on('error', close);
    client.on('close', close); remote.on('close', close);
  });
  const control = http.createServer(async (request, response) => {
    try {
      if (request.method !== 'POST') throw new Error('POST required');
      const action = request.url.slice(1);
      if (action === 'hold-upstream') held.upstream = true;
      else if (action === 'hold-downstream') held.downstream = true;
      else if (action === 'release') held = { upstream: false, downstream: false };
      else if (action === 'disconnect') {
        for (const { client, remote } of pairs) { client.destroy(); remote.destroy(); }
      } else if (action !== 'status' && action !== 'stop') throw new Error('unknown proxy command');
      for (const { client, remote } of pairs) {
        held.upstream ? client.pause() : client.resume();
        held.downstream ? remote.pause() : remote.resume();
      }
      response.setHeader('content-type', 'application/json');
      response.end(JSON.stringify({ held, connections: pairs.size,
        bufferedUpstream: [...pairs].reduce((sum, p) => sum + p.client.readableLength, 0),
        bufferedDownstream: [...pairs].reduce((sum, p) => sum + p.remote.readableLength, 0) }));
      if (action === 'stop') setImmediate(() => close());
    } catch (error) { response.writeHead(400); response.end(JSON.stringify({ error: error.message })); }
  });
  const listen = (socket, port) => new Promise((resolve, reject) => {
    socket.once('error', reject); socket.listen(port, '127.0.0.1', resolve);
  });
  const close = async () => {
    for (const { client, remote } of pairs) { client.destroy(); remote.destroy(); }
    await Promise.all([server, control].map((s) => new Promise((r) => s.close(r))));
  };
  try { await listen(server, listenPort); await listen(control, controlPort); }
  catch (error) { await close(); throw error; }
  const state = { pid: process.pid, endpoint: `http://127.0.0.1:${server.address().port}`,
    control: `http://127.0.0.1:${control.address().port}`, target };
  if (stateFile) await writeJson(stateFile, state);
  return { ...state, close };
}
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) main(async () => {
  const options = args({ target: 'string', listen: 'string', control: 'string', 'state-file': 'string' });
  if (options.command === 'serve') {
    const proxy = await startProxy({ target: options.target, listenPort: integer(options.listen ?? 0, 'listen', 0, LIMITS.maximumPort),
      controlPort: integer(options.control ?? 0, 'control', 0, LIMITS.maximumPort), stateFile: options['state-file'] });
    console.log(JSON.stringify({ endpoint: proxy.endpoint, control: proxy.control, pid: proxy.pid }));
    for (const signal of ['SIGTERM', 'SIGINT']) process.on(signal, () => proxy.close());
  } else {
    if (!options.control) throw new Error('--control URL is required');
    console.log(JSON.stringify(await rpc(options.control, `/${options.command}`)));
  }
});

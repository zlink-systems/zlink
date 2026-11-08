#!/usr/bin/env node
// Only native etcd's Raft HTTP transport passes here. Client RPCs use separate listeners.
import http from 'node:http';
import { args, main, readJson } from './common.mjs';

main(async () => {
  const options = args({ config: 'string' });
  const config = await readJson(options.config);
  const connections = new Set();
  let partitioned = new Set();
  const servers = config.members.map((member) => {
    const server = http.createServer((incoming, outgoing) => {
      // etcd v3.6.16 rafthttp sends X-Server-From on POST and the source ID in stream URLs.
      const source = incoming.headers['x-server-from'] ?? incoming.url.match(/\/raft\/stream\/[^/]+\/([0-9a-f]+)$/)?.[1];
      if (partitioned.has(member.memberId) || partitioned.has(source)) { incoming.socket.destroy(); return; }
      const forwarded = http.request({ hostname: '127.0.0.1', port: member.peerBackendPort,
        path: incoming.url, method: incoming.method, headers: incoming.headers }, (reply) => {
        outgoing.writeHead(reply.statusCode, reply.headers); outgoing.flushHeaders(); reply.pipe(outgoing);
      });
      const record = { source, target: member.memberId, incoming, outgoing, forwarded };
      connections.add(record);
      const close = () => { connections.delete(record); forwarded.destroy(); incoming.destroy(); outgoing.destroy(); };
      forwarded.on('error', close); outgoing.on('close', close); incoming.on('error', close);
      incoming.pipe(forwarded);
    });
    server.requestTimeout = 0; server.timeout = 0;
    return { server, port: member.peerPort };
  });
  const control = http.createServer(async (request, response) => {
    try {
      if (request.method !== 'POST') throw new Error('POST required');
      const url = new URL(request.url, 'http://localhost');
      if (url.pathname === '/partition') partitioned.add(url.searchParams.get('id'));
      else if (url.pathname === '/heal') partitioned.clear();
      else if (url.pathname === '/configure') {
        const latest = await readJson(options.config);
        for (let i = 0; i < config.members.length; i++) config.members[i].memberId = latest.members[i].memberId;
      } else if (url.pathname !== '/status') throw new Error('unknown peer proxy command');
      for (const record of connections) if (partitioned.has(record.source) || partitioned.has(record.target)) {
        record.forwarded.destroy(); record.incoming.destroy(); record.outgoing.destroy();
      }
      response.end(JSON.stringify({ partitioned: [...partitioned] }));
    } catch (error) { response.writeHead(400); response.end(JSON.stringify({ error: error.message })); }
  });
  servers.push({ server: control, port: config.peerControlPort });
  for (const { server, port } of servers) await new Promise((resolve, reject) => {
    server.once('error', reject); server.listen(port, '127.0.0.1', resolve);
  });
  const shutdown = () => {
    for (const record of connections) { record.forwarded.destroy(); record.incoming.destroy(); record.outgoing.destroy(); }
    for (const { server } of servers) { server.closeAllConnections(); server.close(); }
  };
  process.on('SIGTERM', shutdown); process.on('SIGINT', shutdown);
});

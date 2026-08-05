import fs from 'node:fs';
import net from 'node:net';

const options = parseOptions(process.argv.slice(2));
const evidence = fs.openSync(options.evidence, 'a');
const sockets = new Set();

const server = net.createServer((incoming) => {
  fs.writeSync(evidence, `accepted=${Date.now()}\n`);
  sockets.add(incoming);
  let closed = false;
  let outgoing;
  const closeIncoming = () => {
    if (closed) return;
    closed = true;
    sockets.delete(incoming);
    incoming.destroy();
    if (outgoing !== undefined) {
      sockets.delete(outgoing);
      outgoing.destroy();
    }
  };
  incoming.once('error', closeIncoming);
  incoming.once('close', closeIncoming);

  const connectUpstream = () => {
    if (closed) return;
    outgoing = net.connect({
      host: options.upstreamHost,
      port: options.upstreamPort
    });
    outgoing.once('connect', () => {
      if (closed) {
        outgoing.destroy();
        return;
      }
      sockets.add(outgoing);
      incoming.pipe(outgoing);
      outgoing.pipe(incoming);
      outgoing.once('close', closeIncoming);
    });
    outgoing.once('error', () => {
      outgoing.destroy();
      if (!closed) setTimeout(connectUpstream, 25);
    });
  };
  connectUpstream();
});

server.listen(options.listenPort, options.listenHost, () => {
  fs.writeFileSync(options.ready, 'ready\n');
});

function shutdown() {
  for (const socket of sockets) socket.destroy();
  server.close(() => {
    fs.closeSync(evidence);
    process.exit(0);
  });
}

process.on('SIGTERM', shutdown);
process.on('SIGINT', shutdown);

function parseOptions(args) {
  const values = new Map();
  for (let index = 0; index < args.length; index += 2) {
    const flag = args[index];
    const value = args[index + 1];
    if (!flag?.startsWith('--') || value === undefined) {
      throw new Error(`Invalid proxy option '${flag ?? ''}'.`);
    }
    values.set(flag.slice(2), value);
  }
  const required = (key) => {
    const value = values.get(key);
    if (value === undefined || value.length === 0) {
      throw new Error(`--${key} is required.`);
    }
    return value;
  };
  return {
    listenHost: values.get('listen-host') ?? '127.0.0.1',
    listenPort: Number(required('listen-port')),
    upstreamHost: values.get('upstream-host') ?? '127.0.0.1',
    upstreamPort: Number(required('upstream-port')),
    evidence: required('evidence'),
    ready: required('ready')
  };
}

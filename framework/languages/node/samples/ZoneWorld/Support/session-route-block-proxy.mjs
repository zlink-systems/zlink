import fs from 'node:fs';
import net from 'node:net';
import process from 'node:process';

const ZMP_MAGIC = 0x5a;
const ZMP_VERSION = 0x01;
const ZMP_HEADER_SIZE = 8;
const ZMP_REQUEST_SEQUENCE_SIZE = 8;
const ZMP_FLAG_MORE = 0x01;
const ZMP_REQUEST_REPLY_KINDS = new Set([0x01, 0x02, 0x03]);
const SESSION_RELOCATION_ROUTE = 44;

const options = parseArguments(process.argv.slice(2));
const sockets = new Set();
const server = net.createServer((downstream) => {
  sockets.add(downstream);
  downstream.once('close', () => sockets.delete(downstream));
  downstream.pause();
  const upstream = net.createConnection({ host: options.targetHost, port: options.targetPort });
  sockets.add(upstream);
  upstream.once('close', () => sockets.delete(upstream));
  upstream.once('connect', () => {
    console.log(
      `proxy-connection listen=${options.listenHost}:${options.listenPort} `
      + `target=${options.targetHost}:${options.targetPort}`
    );
    pump(downstream, upstream, 'peer-to-gateway');
    pump(upstream, downstream, 'gateway-to-peer');
    downstream.resume();
  });
  upstream.once('error', (error) => {
    console.log(`proxy-pump-ended direction=connect error=${error.message}`);
    downstream.destroy();
  });
});

server.once('error', (error) => {
  console.error(error);
  process.exitCode = 1;
});
server.listen(options.listenPort, options.listenHost, () => {
  console.log(
    `proxy-ready listen=${options.listenHost}:${options.listenPort} `
    + `target=${options.targetHost}:${options.targetPort}`
  );
});

function pump(source, sink, direction) {
  let buffer = Buffer.alloc(0);
  let message = [];
  let blocked = false;
  source.on('data', (data) => {
    buffer = buffer.length === 0 ? data : Buffer.concat([buffer, data]);
    try {
      while (buffer.length >= ZMP_HEADER_SIZE) {
        if (buffer[0] !== ZMP_MAGIC || buffer[1] !== ZMP_VERSION) {
          throw new Error('unexpected ZMP frame header');
        }
        const flags = buffer[2];
        const kind = buffer[3];
        const bodySize = buffer.readUInt32BE(4);
        const headerSize = ZMP_HEADER_SIZE
          + (ZMP_REQUEST_REPLY_KINDS.has(kind) ? ZMP_REQUEST_SEQUENCE_SIZE : 0);
        const totalSize = headerSize + bodySize;
        if (buffer.length < totalSize) return;
        const frame = buffer.subarray(0, totalSize);
        const body = frame.subarray(headerSize);
        buffer = buffer.subarray(totalSize);
        message.push(frame);
        blocked ||= fs.existsSync(options.armFile) && isSessionRelocationRoute(body);
        if ((flags & ZMP_FLAG_MORE) !== 0) continue;
        if (blocked) {
          console.log(`blocked-command-44 direction=${direction}`);
        } else {
          sink.write(Buffer.concat(message));
        }
        message = [];
        blocked = false;
      }
    } catch (error) {
      console.log(`proxy-pump-ended direction=${direction} error=${error.message}`);
      source.destroy();
      sink.destroy();
    }
  });
  source.once('end', () => sink.end());
  source.once('error', (error) => {
    console.log(`proxy-pump-ended direction=${direction} error=${error.message}`);
    sink.destroy();
  });
}

function isSessionRelocationRoute(body) {
  return body.length >= 5 && body[0] === 90 && body[1] === 77
    && body[3] === SESSION_RELOCATION_ROUTE;
}

function parseArguments(args) {
  const values = new Map();
  for (let index = 0; index < args.length; index += 2) {
    const key = args[index];
    const value = args[index + 1];
    if (!key?.startsWith('--') || value === undefined) {
      throw new Error(`Invalid proxy argument '${key ?? ''}'.`);
    }
    values.set(key.slice(2), value);
  }
  const required = ['listen-host', 'listen-port', 'target-host', 'target-port', 'arm-file'];
  for (const key of required) {
    if (!values.has(key)) throw new Error(`--${key} is required.`);
  }
  return {
    listenHost: values.get('listen-host'),
    listenPort: parsePort(values.get('listen-port'), 'listen-port'),
    targetHost: values.get('target-host'),
    targetPort: parsePort(values.get('target-port'), 'target-port'),
    armFile: values.get('arm-file')
  };
}

function parsePort(value, name) {
  const port = Number.parseInt(value, 10);
  if (!Number.isInteger(port) || port < 1 || port > 65535) {
    throw new Error(`--${name} must be a TCP port.`);
  }
  return port;
}

function stop() {
  for (const socket of sockets) socket.destroy();
  server.close(() => process.exit(0));
}

process.once('SIGINT', stop);
process.once('SIGTERM', stop);

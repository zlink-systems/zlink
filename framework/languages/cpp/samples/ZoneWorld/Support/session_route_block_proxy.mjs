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
    pump(downstream, upstream, 'peer-to-gateway');
    pump(upstream, downstream, 'gateway-to-peer');
    downstream.resume();
  });
  upstream.once('error', () => downstream.destroy());
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
  let command44 = null;
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
        command44 ??= command44Identity(body);
        if ((flags & ZMP_FLAG_MORE) !== 0) continue;
        const blocked = message.length === 1 && fs.existsSync(options.armFile)
          && command44 !== null && command44.action === 1;
        if (blocked) {
          fs.closeSync(fs.openSync(`${options.armFile}.blocked`, 'a'));
          console.log(
            `blocked-command-44 direction=${direction} actor=${command44.actor} action=commit `
            + `previous-authority=${command44.previous} target-authority=${command44.target}`
          );
        } else {
          sink.write(Buffer.concat(message));
        }
        message = [];
        command44 = null;
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

function command44Identity(body) {
  if (body.length < 5 || body[0] !== 90 || body[1] !== 77
      || body[3] !== SESSION_RELOCATION_ROUTE) return null;
  let offset = 21;
  const text8 = () => {
    const size = body[offset];
    offset += 1;
    const value = body.subarray(offset, offset + size);
    offset += size;
    return value;
  };
  const text16 = () => {
    const size = body.readUInt16BE(offset);
    offset += 2 + size;
  };
  try {
    text8();
    offset += 8;
    text8();
    offset += 8;
    text16();
    offset += 1;
    const actor = text8().toString('utf8');
    offset += 8;
    text8();
    offset += 8;
    text8();
    offset += 8;
    text8();
    offset += 8;
    const action = body[offset];
    offset += 1;
    const routeSize = body.readUInt16BE(offset);
    offset += 2;
    const previous = action === 1 && routeSize >= 16 ? body.readBigUInt64BE(offset) : 0n;
    const target = action === 1 && routeSize >= 16 ? body.readBigUInt64BE(offset + 8) : 0n;
    return { actor, action, previous, target };
  } catch {
    return null;
  }
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
  for (const key of ['listen-host', 'listen-port', 'target-host', 'target-port', 'arm-file']) {
    if (!values.has(key)) throw new Error(`--${key} is required.`);
  }
  return {
    listenHost: values.get('listen-host'),
    listenPort: Number.parseInt(values.get('listen-port'), 10),
    targetHost: values.get('target-host'),
    targetPort: Number.parseInt(values.get('target-port'), 10),
    armFile: values.get('arm-file')
  };
}

function stop() {
  for (const socket of sockets) socket.destroy();
  server.close(() => process.exit(0));
}

process.once('SIGINT', stop);
process.once('SIGTERM', stop);

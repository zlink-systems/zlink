const readline = require('node:readline');
const path = require('node:path');
const { ZLinkEtcdLocationStore } = require(
  process.env.ZLINK_ETCD_PROVIDER_DIST ||
    path.resolve(__dirname, '../../../packages/framework-locations-etcd/dist')
);
const stores = new Map();
const operations = new Map();
const seen = new Set();
const key = (value) => ({ value });
function decimal(value) {
  if (
    typeof value !== 'string' ||
    !/^-?(0|[1-9][0-9]*)$/.test(value) ||
    !Number.isSafeInteger(Number(value))
  )
    throw new Error('expected integer decimal milliseconds');
  return Number(value);
}
function bytes(value) {
  if (typeof value !== 'string' || Buffer.from(value, 'base64').toString('base64') !== value)
    throw new Error('expected canonical base64');
  return Buffer.from(value, 'base64');
}
function value(v) {
  return {
    bytesBase64: Buffer.from(v.bytes).toString('base64'),
    version: v.version.value,
    expiresAtMs: v.expiresAt === undefined ? null : String(v.expiresAt.getTime()),
    storeNowMs: String(v.storeNow.getTime())
  };
}
function result(r) {
  switch (r.kind) {
    case 'found':
      return { kind: r.kind, value: value(r.value) };
    case 'missing':
    case 'conflict':
      return { kind: r.kind, storeNowMs: String(r.storeNow.getTime()) };
    case 'applied':
      return {
        kind: r.kind,
        storeNowMs: String(r.storeNow.getTime()),
        putVersions: r.putVersions.map((p) => ({ key: p.key.value, version: p.version.value }))
      };
    case 'expired':
      return { kind: r.kind };
    case 'page':
      return {
        kind: r.kind,
        value: {
          storeNowMs: String(r.value.storeNow.getTime()),
          nextCursor: r.value.nextCursor?.value ?? null,
          items: r.value.items.map((i) => ({ key: i.key.value, value: value(i.value) }))
        }
      };
    default:
      throw new Error(`unknown public result: ${r.kind}`);
  }
}
function prepare(m) {
  if (m.op === 'write')
    return {
      conditions: m.request.conditions.map((c) => ({
        kind: c.kind,
        key: key(c.key),
        ...(c.kind === 'missing'
          ? {}
          : { expected: c.kind === 'version' ? key(c.version) : bytes(c.bytesBase64) })
      })),
      mutations: m.request.mutations.map((x) => ({
        kind: x.kind,
        key: key(x.key),
        ...(x.kind === 'put'
          ? {
              bytes: bytes(x.bytesBase64),
              ...(x.retentionMs === undefined ? {} : { retentionMs: decimal(x.retentionMs) })
            }
          : {})
      }))
    };
  if (m.op === 'scan')
    return { ...m.request, cursor: m.request.cursor == null ? undefined : key(m.request.cursor) };
  return undefined;
}
function reply(id, body) {
  process.stdout.write(`${JSON.stringify({ v: 1, id, ...body })}\n`);
}
async function dispatch(m) {
  let phase = 'adapter';
  try {
    if (m.v !== 1 || typeof m.id !== 'string' || seen.has(m.id))
      throw new Error('invalid protocol version or duplicate request id');
    seen.add(m.id);
    if (m.op === 'cancel') {
      const controller = operations.get(m.target);
      controller?.abort();
      reply(m.id, { ok: true, result: { kind: 'cancelled', active: !!controller } });
      return;
    }
    if (m.op === 'create') {
      if (stores.has(m.store)) throw new Error('duplicate store');
      phase = 'provider';
      stores.set(m.store, new ZLinkEtcdLocationStore(m.options));
      reply(m.id, { ok: true, result: { kind: 'created' } });
      return;
    }
    const store = stores.get(m.store);
    if (!store) throw new Error('unknown store');
    const request = prepare(m);
    if (!['read', 'write', 'scan', 'dispose'].includes(m.op)) throw new Error('unknown operation');
    const controller = new AbortController();
    operations.set(m.id, controller);
    phase = 'provider';
    let r;
    if (m.op === 'dispose') {
      await store.dispose();
      stores.delete(m.store);
      r = { kind: 'disposed' };
    } else {
      const native =
        m.op === 'read'
          ? await store.read(key(m.key), controller.signal)
          : await store[m.op](request, controller.signal);
      phase = 'adapter';
      r = result(native);
    }
    reply(m.id, { ok: true, result: r });
  } catch (error) {
    const category =
      phase === 'adapter'
        ? 'adapter-error'
        : error instanceof TypeError || error instanceof RangeError
          ? 'argument-error'
          : 'store-failure';
    reply(m.id ?? null, {
      ok: false,
      error: {
        category,
        ...(category === 'store-failure' ? { commit: m.op === 'write' ? 'unknown' : 'none' } : {}),
        nativeType: error.name,
        message: error.message
      }
    });
  } finally {
    operations.delete(m.id);
  }
}
const input = readline.createInterface({ input: process.stdin, crlfDelay: Infinity });
const pending = new Set();
input.on('line', (line) => {
  let m;
  try {
    m = JSON.parse(line);
  } catch (error) {
    reply(null, { ok: false, error: { category: 'adapter-error', message: error.message } });
    return;
  }
  const task = dispatch(m);
  pending.add(task);
  task.finally(() => pending.delete(task));
});
input.on('close', async () => {
  await Promise.allSettled([...pending]);
  await Promise.allSettled([...stores.values()].map((s) => s.dispose()));
});

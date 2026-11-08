const { ZLinkEtcdLocationStore } = require('../../../packages/framework-locations-etcd/dist');
function plain(value) {
  if (value instanceof Date) return value.toISOString();
  if (value instanceof Uint8Array) return Array.from(value);
  if (Array.isArray(value)) return value.map(plain);
  if (value !== null && typeof value === 'object')
    return Object.fromEntries(
      Object.entries(value)
        .filter(([, v]) => v !== undefined)
        .map(([k, v]) => [k, plain(v)])
    );
  return value;
}
process.once('message', async ({ options, subject, prefix }) => {
  const store = new ZLinkEtcdLocationStore(options);
  try {
    const key = { value: subject };
    const read = await store.read(key);
    const write = await store.write({
      conditions: [{ kind: 'value', key, expected: Buffer.from([0, 255, 1]) }],
      mutations: []
    });
    const scan = await store.scan({ prefix, limit: 1 });
    let next;
    if (scan.value?.nextCursor)
      next = await store.scan({ prefix, limit: 1, cursor: scan.value.nextCursor });
    if (scan.value) delete scan.value.nextCursor;
    if (next?.value) delete next.value.nextCursor;
    process.send({ result: plain({ read, write, scan, next }) });
  } catch (error) {
    process.send({ error: error.message });
    process.exitCode = 1;
  } finally {
    await store.dispose();
    process.disconnect();
  }
});

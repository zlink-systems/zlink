const original = Date.now;
let offset = Number(process.env.ZLINK_ETCD_WALL_OFFSET || 0);
Date.now = () => original() + offset;
process.on('message', (message) => {
  if (message.kind === 'wall') {
    offset = message.offset;
    process.send?.({ kind: 'wall-set', offset });
  }
});
// Report the management process's real RPC boundary without replacing its clock engine.
if (process.env.ZLINK_ETCD_MANAGER_EVENTS === '1') {
  process.channel?.unref();
  const {
    systemTime
  } = require('../../../packages/framework-locations-etcd/dist/internal/admin-engine');
  const sleep = systemTime.sleep;
  systemTime.sleep = (ms, signal) => {
    process.send?.({ kind: 'sleep', ms });
    return sleep(ms, signal);
  };
}

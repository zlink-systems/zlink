const original = Date.now;
let offset = Number(process.env.ZLINK_ETCD_WALL_OFFSET || 0);
Date.now = () => original() + offset;
process.on('message', (message) => {
  if (message.kind === 'wall') {
    offset = message.offset;
    process.send?.({ kind: 'wall-set', offset });
  }
});
process.channel?.unref();

const { cpSync } = require('node:fs');
const { resolve } = require('node:path');
cpSync(resolve(__dirname, '../../../../runtime/protocol/etcd'), resolve(__dirname, 'dist/proto'), { recursive: true });

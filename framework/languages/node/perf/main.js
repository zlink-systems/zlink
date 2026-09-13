#!/usr/bin/env node
'use strict';
require('reflect-metadata');
const fs = require('node:fs');

async function main() {
  const args = process.argv.slice(2);
  if (args.length === 4 && args[0] === '--endpoint-config' && args[2] === '--client-index') {
    await require('./Client/main').runClient(JSON.parse(fs.readFileSync(args[1], 'utf8')), Number(args[3]));
    return;
  }
  if (args.length !== 2 || args[0] !== '--config') throw new Error('Use --config <role.json> or --endpoint-config <endpoints.json> --client-index <index>.');
  const config = JSON.parse(fs.readFileSync(args[1], 'utf8'));
  if (config.role === 'client') { await require('./Client/main').runClient(config.endpointManifest, config.roleInstance); return; }
  const application = await require('./Server/application').createApplication(config);
  let closing = false;
  async function close() { if (closing) return; closing = true; await application.close(); }
  for (const signal of ['SIGINT', 'SIGTERM']) process.once(signal, () => { close().catch(error => { console.error(error); process.exitCode = 1; }); });
}
main().catch(error => { console.error(error); process.exitCode = 1; });

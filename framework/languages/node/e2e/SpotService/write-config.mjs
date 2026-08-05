import fs from 'node:fs';

const [output, ...args] = process.argv.slice(2);
if (!output) throw new Error('Usage: node write-config.mjs <output> [typed fields].');

const e2e = {};
for (let index = 0; index < args.length; index += 1) {
  const kind = args[index];
  const key = args[++index];
  const value = args[++index];
  if (!kind || !key || value === undefined) throw new Error('Each configuration field requires a kind, key, and value.');
  if (kind === '--string') {
    e2e[key] = value;
  } else if (kind === '--array') {
    e2e[key] = value.length === 0 ? [] : value.split(',');
  } else if (kind === '--boolean') {
    if (value !== 'true' && value !== 'false') throw new Error(`Boolean field '${key}' must be true or false.`);
    e2e[key] = value === 'true';
  } else {
    throw new Error(`Unknown configuration field kind '${kind}'.`);
  }
}

fs.writeFileSync(output, `${JSON.stringify({ e2e }, null, 2)}\n`, { mode: 0o600 });
fs.chmodSync(output, 0o600);

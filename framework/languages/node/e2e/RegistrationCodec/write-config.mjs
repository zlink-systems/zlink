import fs from 'node:fs';

const [output, ...args] = process.argv.slice(2);
if (!output) throw new Error('Usage: node write-config.mjs <output> [--key value ...]');
const e2e = {};
for (let index = 0; index < args.length; index += 2) {
  const flag = args[index];
  const value = args[index + 1];
  if (!flag?.startsWith('--') || value === undefined) throw new Error(`Invalid configuration argument '${flag}'.`);
  const key = flag.slice(2).replace(/-([a-z])/g, (_match, letter) => letter.toUpperCase());
  e2e[key] = value;
}
fs.writeFileSync(output, `${JSON.stringify({ e2e }, null, 2)}\n`, { mode: 0o600 });

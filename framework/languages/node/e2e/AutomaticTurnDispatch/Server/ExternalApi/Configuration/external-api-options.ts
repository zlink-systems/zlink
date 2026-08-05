import fs from 'node:fs';
import { objectValue, requiredString } from '../../../configuration';

export interface ExternalApiOptions {
  readonly httpUrl: string;
}

export function readExternalApiOptions(args: readonly string[]): ExternalApiOptions {
  if (args.length !== 2 || args[0] !== '--config' || args[1].startsWith('--')) {
    throw new Error('--config <path> is the only supported external API argument.');
  }
  const document = JSON.parse(fs.readFileSync(args[1], 'utf8')) as { e2e?: unknown };
  if (document.e2e === undefined) throw new Error("Configuration section 'e2e' is required.");
  return { httpUrl: requiredString(objectValue(document.e2e), 'httpUrl') };
}

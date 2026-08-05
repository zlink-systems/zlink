export interface ClientOptions {
  readonly serverUrl: string;
  readonly jsonOnlyUrl: string;
  readonly codecRequesterUrl: string;
  readonly invalidMain: string;
  readonly invalidDuplicateConfig: string;
  readonly invalidHandlerGroupConfig: string;
  readonly invalidChannelKindsConfig: string;
  readonly logDir: string;
  readonly scenario: string;
}

export function parseClientOptions(args: readonly string[]): ClientOptions {
  const values = readConfig(args);
  return {
    serverUrl: required(values, 'serverUrl'),
    jsonOnlyUrl: required(values, 'jsonOnlyUrl'),
    codecRequesterUrl: required(values, 'codecRequesterUrl'),
    invalidMain: required(values, 'invalidMain'),
    invalidDuplicateConfig: required(values, 'invalidDuplicateConfig'),
    invalidHandlerGroupConfig: required(values, 'invalidHandlerGroupConfig'),
    invalidChannelKindsConfig: required(values, 'invalidChannelKindsConfig'),
    logDir: required(values, 'logDir'),
    scenario: typeof values.scenario === 'string' ? values.scenario : 'all'
  };
}

function readConfig(args: readonly string[]): Record<string, unknown> {
  if (args.length !== 2 || args[0] !== '--config' || args[1].startsWith('--')) throw new Error('--config <path> is required.');
  const document = JSON.parse(fs.readFileSync(args[1], 'utf8')) as { e2e?: unknown };
  if (document.e2e === null || typeof document.e2e !== 'object' || Array.isArray(document.e2e)) throw new Error("Configuration section 'e2e' must be an object.");
  return document.e2e as Record<string, unknown>;
}

function required(values: Record<string, unknown>, name: string): string {
  const value = values[name];
  if (typeof value !== 'string' || value.length === 0) throw new Error(`Configuration value 'e2e.${name}' is required.`);
  return value;
}
import fs from 'node:fs';

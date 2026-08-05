export interface ClientOptions {
  readonly topologyUrl: string;
  readonly consumerUrl: string;
  readonly providerAUrl: string;
  readonly providerBUrl?: string;
  readonly scenario: string;
}

export function parseClientOptions(args: readonly string[]): ClientOptions {
  const values = readConfig(args);
  return {
    topologyUrl: required(values, 'topologyUrl'),
    consumerUrl: required(values, 'consumerUrl'),
    providerAUrl: required(values, 'providerAUrl'),
    providerBUrl: optional(values, 'providerBUrl'),
    scenario: optional(values, 'scenario') ?? 'all'
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

function optional(values: Record<string, unknown>, name: string): string | undefined {
  const value = values[name];
  if (value === undefined) return undefined;
  if (typeof value !== 'string') throw new Error(`Configuration value 'e2e.${name}' must be a string.`);
  return value;
}
import fs from 'node:fs';

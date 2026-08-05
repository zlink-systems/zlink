export interface ClientOptions {
  readonly providerAUrl: string;
  readonly providerBUrl: string;
  readonly workflowUrl: string;
  readonly directConsumerUrl: string;
  readonly singleConsumerUrl: string;
  readonly backpressureConsumerUrl: string;
  readonly locationConsumerUrl: string;
  readonly manualConsumerUrl?: string;
  readonly providerMain: string;
  readonly consumerMain: string;
  readonly redisEndpoint: string;
  readonly redisKeyPrefix: string;
  readonly logDir: string;
  readonly scenario: string;
  readonly rmA3ClientAUrl?: string;
  readonly rmA3ClientBUrl?: string;
  readonly rmA3ExpectedState?: string;
  readonly rmA3ExpectedReady?: boolean;
  readonly rmA3StableMilliseconds?: number;
  readonly rmA3CheckNodeDirect?: boolean;
  readonly rmA3ExpectedServerWeight?: number;
}

export function parseClientOptions(args: readonly string[]): ClientOptions {
  const values = readConfig(args);
  return {
    providerAUrl: required(values, 'providerAUrl'),
    providerBUrl: required(values, 'providerBUrl'),
    workflowUrl: required(values, 'workflowUrl'),
    directConsumerUrl: required(values, 'directConsumerUrl'),
    singleConsumerUrl: required(values, 'singleConsumerUrl'),
    backpressureConsumerUrl: required(values, 'backpressureConsumerUrl'),
    locationConsumerUrl: required(values, 'locationConsumerUrl'),
    manualConsumerUrl: optional(values, 'manualConsumerUrl'),
    providerMain: required(values, 'providerMain'),
    consumerMain: required(values, 'consumerMain'),
    redisEndpoint: required(values, 'redisEndpoint'),
    redisKeyPrefix: required(values, 'redisKeyPrefix'),
    logDir: required(values, 'logDir'),
    scenario: typeof values.scenario === 'string' ? values.scenario : 'all',
    rmA3ClientAUrl: optional(values, 'rmA3ClientAUrl'),
    rmA3ClientBUrl: optional(values, 'rmA3ClientBUrl'),
    rmA3ExpectedState: optional(values, 'rmA3ExpectedState'),
    rmA3ExpectedReady: optionalBoolean(values, 'rmA3ExpectedReady'),
    rmA3StableMilliseconds: optionalInteger(values, 'rmA3StableMilliseconds'),
    rmA3CheckNodeDirect: optionalBoolean(values, 'rmA3CheckNodeDirect'),
    rmA3ExpectedServerWeight: optionalInteger(values, 'rmA3ExpectedServerWeight')
  };
}

function readConfig(args: readonly string[]): Record<string, unknown> {
  if (args.length !== 2 || args[0] !== '--config' || args[1].startsWith('--')) throw new Error('--config <path> is required.');
  const document = JSON.parse(fs.readFileSync(args[1], 'utf8')) as { e2e?: unknown };
  if (document.e2e === null || typeof document.e2e !== 'object' || Array.isArray(document.e2e)) throw new Error("Configuration section 'e2e' must be an object.");
  return document.e2e as Record<string, unknown>;
}

function required(values: Record<string, unknown>, key: string): string {
  const value = values[key];
  if (typeof value !== 'string' || value.length === 0) throw new Error(`Configuration value 'e2e.${key}' is required.`);
  return value;
}

function optional(values: Record<string, unknown>, key: string): string | undefined {
  const value = values[key];
  return typeof value === 'string' && value.length > 0 ? value : undefined;
}

function optionalBoolean(values: Record<string, unknown>, key: string): boolean | undefined {
  const value = values[key];
  if (value === undefined) return undefined;
  if (value === true || value === 'true') return true;
  if (value === false || value === 'false') return false;
  throw new Error(`Configuration value 'e2e.${key}' must be boolean.`);
}

function optionalInteger(values: Record<string, unknown>, key: string): number | undefined {
  const value = values[key];
  if (value === undefined) return undefined;
  const parsed = Number(value);
  if (!Number.isInteger(parsed) || parsed < 0) {
    throw new Error(`Configuration value 'e2e.${key}' must be a non-negative integer.`);
  }
  return parsed;
}
import fs from 'node:fs';

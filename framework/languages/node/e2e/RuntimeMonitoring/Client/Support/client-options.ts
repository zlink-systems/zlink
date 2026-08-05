export interface ClientOptions {
  readonly triggerUrl: string;
  readonly serviceUrl: string;
  readonly serviceBUrl: string;
  readonly throwServiceUrl: string;
  readonly redisEndpoint: string;
  readonly redisKeyPrefix: string;
  readonly redisContainer: string;
  readonly serviceBChannelEndpoint: string;
  readonly serviceChannelEndpoint: string;
  readonly serviceBSpotRouterEndpoint: string;
  readonly serviceBSpotPubEndpoint: string;
  readonly serviceMain: string;
  readonly filteredServiceMain: string;
  readonly serviceBConfig: string;
  readonly replacementServiceUrl: string;
  readonly replacementServiceChannelEndpoint: string;
  readonly replacementServiceConfig: string;
  readonly logDir: string;
  readonly scenario: string;
}

export function parseClientOptions(args: readonly string[]): ClientOptions {
  const values = readConfig(args);
  return {
    triggerUrl: required(values, 'triggerUrl'),
    serviceUrl: required(values, 'serviceUrl'),
    serviceBUrl: required(values, 'serviceBUrl'),
    throwServiceUrl: required(values, 'throwServiceUrl'),
    redisEndpoint: required(values, 'redisEndpoint'),
    redisKeyPrefix: required(values, 'redisKeyPrefix'),
    redisContainer: required(values, 'redisContainer'),
    serviceBChannelEndpoint: required(values, 'serviceBChannelEndpoint'),
    serviceChannelEndpoint: required(values, 'serviceChannelEndpoint'),
    serviceBSpotRouterEndpoint: required(values, 'serviceBSpotRouterEndpoint'),
    serviceBSpotPubEndpoint: required(values, 'serviceBSpotPubEndpoint'),
    serviceMain: required(values, 'serviceMain'),
    filteredServiceMain: required(values, 'filteredServiceMain'),
    serviceBConfig: required(values, 'serviceBConfig'),
    replacementServiceUrl: required(values, 'replacementServiceUrl'),
    replacementServiceChannelEndpoint: required(values, 'replacementServiceChannelEndpoint'),
    replacementServiceConfig: required(values, 'replacementServiceConfig'),
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

function required(values: Record<string, unknown>, key: string): string {
  const value = values[key];
  if (typeof value !== 'string' || value.length === 0) throw new Error(`Configuration value 'e2e.${key}' is required.`);
  return value;
}
import fs from 'node:fs';

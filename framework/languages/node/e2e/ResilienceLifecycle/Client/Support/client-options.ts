export interface ClientOptions {
  readonly peerLocationUrl: string;
  readonly providerAUrl: string;
  readonly providerBUrl: string;
  readonly consumerUrl: string;
  readonly consumerUrls: readonly string[];
  readonly soakDurationMs: number;
  readonly redisEndpoint: string;
  readonly redisKeyPrefix: string;
  readonly redisContainer: string;
  readonly providerAChannelEndpoint: string;
  readonly providerBChannelEndpoint: string;
  readonly providerBRemapUrl: string;
  readonly providerBRemapChannelEndpoint: string;
  readonly providerBGreenUrl: string;
  readonly providerBGreenChannelEndpoint: string;
  readonly providerMain: string;
  readonly logDir: string;
  readonly scenario: string;
}

export function parseClientOptions(args: readonly string[]): ClientOptions {
  const values = readConfig(args);
  const soakDurationSeconds = Number(required(values, 'soakDurationSeconds'));
  if (!Number.isInteger(soakDurationSeconds) || soakDurationSeconds < 120) {
    throw new Error('--soak-duration-seconds must be an integer of at least 120.');
  }
  return {
    peerLocationUrl: required(values, 'peerLocationUrl'),
    providerAUrl: required(values, 'providerAUrl'),
    providerBUrl: required(values, 'providerBUrl'),
    consumerUrl: required(values, 'consumerUrl'),
    consumerUrls: required(values, 'consumerUrls').split(',').filter((value) => value.length > 0),
    soakDurationMs: soakDurationSeconds * 1000,
    redisEndpoint: required(values, 'redisEndpoint'),
    redisKeyPrefix: required(values, 'redisKeyPrefix'),
    redisContainer: required(values, 'redisContainer'),
    providerAChannelEndpoint: required(values, 'providerAChannelEndpoint'),
    providerBChannelEndpoint: required(values, 'providerBChannelEndpoint'),
    providerBRemapUrl: required(values, 'providerBRemapUrl'),
    providerBRemapChannelEndpoint: required(values, 'providerBRemapChannelEndpoint'),
    providerBGreenUrl: required(values, 'providerBGreenUrl'),
    providerBGreenChannelEndpoint: required(values, 'providerBGreenChannelEndpoint'),
    providerMain: required(values, 'providerMain'),
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
  if ((typeof value !== 'string' && typeof value !== 'number') || String(value).length === 0) throw new Error(`Configuration value 'e2e.${key}' is required.`);
  return String(value);
}
import fs from 'node:fs';

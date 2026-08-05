import { objectValues, optional } from '../../Shared/Configuration/server-options';

export const SUBSCRIBER_OPTIONS = Symbol.for('@zlink-systems/e2e-pubsub:subscriber-options');

export interface SubscriberOptions {
  readonly rid: string;
  readonly httpUrl: string;
  readonly logDir: string;
  readonly evidenceFile?: string;
  readonly handlerDelayMs: number;
  readonly channelName?: string;
  readonly publisherEndpoint?: string;
  readonly subscriberMode?: 'mixed';
  readonly redisEndpoint?: string;
  readonly redisKeyPrefix?: string;
}

export function validateSubscriberOptions(value: unknown): SubscriberOptions {
  const values = objectValues(value);
  const handlerDelayMs = values.handlerDelayMs === undefined ? 0 : Number(values.handlerDelayMs);
  if (!Number.isFinite(handlerDelayMs) || handlerDelayMs < 0) throw new Error("Configuration value 'e2e.handlerDelayMs' must be a non-negative number.");
  const redisEndpoint = optional(values, 'redisEndpoint');
  const redisKeyPrefix = optional(values, 'redisKeyPrefix');
  if ((redisEndpoint === undefined) !== (redisKeyPrefix === undefined)) {
    throw new Error("Configuration values 'e2e.redisEndpoint' and 'e2e.redisKeyPrefix' must be provided together.");
  }
  return {
    rid: optional(values, 'rid') ?? 'subscriber',
    httpUrl: optional(values, 'httpUrl') ?? 'http://127.0.0.1:0',
    logDir: optional(values, 'logDir') ?? 'logs',
    evidenceFile: optional(values, 'evidenceFile'),
    handlerDelayMs,
    channelName: optional(values, 'channelName'),
    publisherEndpoint: optional(values, 'publisherEndpoint'),
    subscriberMode: optional(values, 'subscriberMode') === 'mixed' ? 'mixed' : undefined,
    redisEndpoint,
    redisKeyPrefix
  };
}

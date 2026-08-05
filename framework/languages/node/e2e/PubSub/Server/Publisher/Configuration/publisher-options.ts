import { objectValues, optional, required } from '../../Shared/Configuration/server-options';

export interface PublisherOptions {
  readonly rid: string;
  readonly httpUrl: string;
  readonly logDir: string;
  readonly publisherEndpoint: string;
  readonly channelName?: string;
  readonly evidenceFile?: string;
  readonly redisEndpoint?: string;
  readonly redisKeyPrefix?: string;
  readonly publisherAdvertiseHost?: string;
  readonly publisherIdentityMode?: 'fixed' | 'automatic' | 'missing' | 'both';
}

export function validatePublisherOptions(value: unknown): PublisherOptions {
  const values = objectValues(value);
  const publisherIdentityMode = optional(values, 'publisherIdentityMode');
  if (publisherIdentityMode !== undefined
    && !['fixed', 'automatic', 'missing', 'both'].includes(publisherIdentityMode)) {
    throw new Error("Configuration value 'e2e.publisherIdentityMode' is invalid.");
  }
  const redisEndpoint = optional(values, 'redisEndpoint');
  const redisKeyPrefix = optional(values, 'redisKeyPrefix');
  if ((redisEndpoint === undefined) !== (redisKeyPrefix === undefined)) {
    throw new Error("Configuration values 'e2e.redisEndpoint' and 'e2e.redisKeyPrefix' must be provided together.");
  }
  return {
    rid: optional(values, 'rid') ?? 'publisher',
    httpUrl: optional(values, 'httpUrl') ?? 'http://127.0.0.1:0',
    logDir: optional(values, 'logDir') ?? 'logs',
    publisherEndpoint: required(values, 'publisherEndpoint'),
    channelName: optional(values, 'channelName'),
    evidenceFile: optional(values, 'evidenceFile'),
    redisEndpoint,
    redisKeyPrefix,
    publisherAdvertiseHost: optional(values, 'publisherAdvertiseHost'),
    publisherIdentityMode: publisherIdentityMode as PublisherOptions['publisherIdentityMode']
  };
}

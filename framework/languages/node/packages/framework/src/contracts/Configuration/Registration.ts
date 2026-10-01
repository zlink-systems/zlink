import { AutoHwmProfile } from '@zlink-systems/zlink';
import type { ZLinkFrameworkOptions } from '../../contracts';
import { ZLINK_MAX_IDENTITY_TEXT_BYTES } from '../Common/CoreTypes';
import { ZLinkApplicationJobQueueProfile } from '../Dispatch';
import { zlinkDefaultLocationOptions } from '../Locations/Options';
import { createFrameworkOptions } from './RegistrationBuilders';
import { createCodecRegistry } from './RegistrationCodecRegistry';
import {
  actorFactoriesFromSpotNodes,
  channelNamesWith,
  normalizeLocationRegistration,
  normalizeNetworkOptions,
  normalizeOptionalPositiveInteger,
  normalizeStreamCompression,
  normalizeWorkerOptions,
  toChannelMap,
  toRouteChannelOptions,
  toSpotFactorySet,
  toSpotNodeMap,
  toSpotPublisherClientSet,
  toStreamNodeMap
} from './RegistrationNormalizers';
import type {
  ZLinkFrameworkRegistration,
  ZLinkFrameworkRegistrationOptions
} from './RegistrationTypes';
import { validateFrameworkRegistration } from './RegistrationValidators';

export const DEFAULT_REQUEST_TIMEOUT_MS = 30_000;
export { ZLinkConfigurationException } from './ConfigurationException';
export { createFrameworkOptions } from './RegistrationBuilders';
export {
  hasActorManager,
  hasSpotNode,
  hasSpotPublisherClient,
  requirePositiveInteger
} from './RegistrationNormalizers';
export * from './RegistrationTypes';
export { validateFrameworkRegistration };
export {
  normalizeEndpoint,
  buildAdvertisedEndpoint,
  parseEndpointHostPort
} from './EndpointNotation';

export const DEFAULT_SESSION_REPLACEMENT_CALLBACK_TIMEOUT_MS = 30_000;

export function createFrameworkRegistration(
  options: ZLinkFrameworkRegistrationOptions = {}
): ZLinkFrameworkRegistration {
  const codecRegistry = createCodecRegistry(options.codecs);
  const network = normalizeNetworkOptions(options.network);
  const routeChannelOptions = toRouteChannelOptions(options);
  const spotNodes = toSpotNodeMap(options.spotNodes, network);
  const registration: ZLinkFrameworkRegistration = {
    network,
    applicationVersion: normalizeApplicationVersion(options.applicationVersion),
    maintenanceWave: normalizeMaintenanceWave(options.maintenanceWave),
    messageSerializers: codecRegistry.registeredSerializers,
    codecs: codecRegistry.registration,
    requestTimeoutMs: normalizeOptionalPositiveInteger(
      options.requestTimeoutMs,
      'requestTimeoutMs'
    ),
    actorFactories: actorFactoriesFromSpotNodes(spotNodes),
    actorTransferTimeoutMs: normalizeOptionalPositiveInteger(
      options.actorTransferTimeoutMs,
      'actorTransferTimeoutMs'
    ),
    messageFollowDurationMs: normalizeNonNegativeInteger(
      options.messageFollowDurationMs,
      'messageFollowDurationMs',
      zlinkDefaultLocationOptions.messageFollowDurationMs
    ),
    sessionReplacementCallbackTimeoutMs: normalizePositiveInteger(
      options.sessionReplacementCallbackTimeoutMs,
      'sessionReplacementCallbackTimeoutMs',
      DEFAULT_SESSION_REPLACEMENT_CALLBACK_TIMEOUT_MS
    ),
    spotFactories: toSpotFactorySet(options.spotFactories, spotNodes),
    channels: toChannelMap(options.channels, network),
    channelClients: channelNamesWith(options.channels, (channel) => channel.client !== undefined),
    fanoutPublishers: channelNamesWith(
      options.channels,
      (channel) => channel.publisher !== undefined
    ),
    routeChannels: new Set(routeChannelOptions.keys()),
    routeChannelOptions,
    streamNodes: toStreamNodeMap(options.streamNodes, network),
    streamCompression: normalizeStreamCompression(options.streamCompression),
    spotNodes,
    spotPublisherClients: toSpotPublisherClientSet(options.spotPublisherClients, spotNodes),
    filterTypes: [...(options.filters ?? [])],
    worker: normalizeWorkerOptions(options.worker),
    dispatch: options.dispatch,
    metrics: options.metrics,
    coreHwm: normalizeCoreHwm(options.coreHwm),
    applicationJobQueue: normalizeApplicationJobQueue(options.applicationJobQueue),
    locations: normalizeLocationRegistration(options.locations)
  };
  validateFrameworkRegistration(registration, options);
  return registration;
}

export const MAX_QUEUED_APPLICATION_JOBS = 2_147_483_647n;
export const DEFAULT_PAUSE_THRESHOLD_PERCENT = 80;
export const DEFAULT_RESUME_THRESHOLD_PERCENT = 60;
export const APPLICATION_JOB_QUEUE_PERCENT_SCALE = 100;

export function isKnownApplicationJobQueueProfile(
  value: unknown
): value is ZLinkApplicationJobQueueProfile {
  return Object.values(ZLinkApplicationJobQueueProfile).includes(
    value as ZLinkApplicationJobQueueProfile
  );
}

export function validateApplicationJobQueueMaximum(
  value: bigint | undefined,
  createError: (message: string) => Error
): void {
  if (
    value !== undefined &&
    (typeof value !== 'bigint' || value < 1n || value > MAX_QUEUED_APPLICATION_JOBS)
  ) {
    throw createError(
      `maxQueuedApplicationJobs must be a bigint in the range 1..${MAX_QUEUED_APPLICATION_JOBS}.`
    );
  }
}

export function validateApplicationJobQueuePressureThresholds(
  pause: number,
  resume: number,
  createError: (message: string) => Error
): void {
  if (!Number.isInteger(pause) || pause < 1 || pause > APPLICATION_JOB_QUEUE_PERCENT_SCALE) {
    throw createError(
      `pauseThresholdPercent must be an integer in the range 1..${APPLICATION_JOB_QUEUE_PERCENT_SCALE}.`
    );
  }
  if (!Number.isInteger(resume) || resume < 0 || resume >= APPLICATION_JOB_QUEUE_PERCENT_SCALE) {
    throw createError(
      `resumeThresholdPercent must be an integer in the range 0..${APPLICATION_JOB_QUEUE_PERCENT_SCALE - 1}.`
    );
  }
  if (resume >= pause) {
    throw createError('resumeThresholdPercent must be less than pauseThresholdPercent.');
  }
}

function normalizeApplicationJobQueue(
  value: ZLinkFrameworkRegistrationOptions['applicationJobQueue']
): NonNullable<ZLinkFrameworkRegistrationOptions['applicationJobQueue']> {
  const profile = value?.profile ?? ZLinkApplicationJobQueueProfile.Balanced;
  if (!isKnownApplicationJobQueueProfile(profile)) {
    throw new TypeError('applicationJobQueue.profile must be a supported profile.');
  }
  const maxQueuedApplicationJobs = value?.maxQueuedApplicationJobs;
  validateApplicationJobQueueMaximum(
    maxQueuedApplicationJobs,
    (message) => new TypeError(`applicationJobQueue.${message}`)
  );
  const pauseThresholdPercent = value?.pauseThresholdPercent ?? DEFAULT_PAUSE_THRESHOLD_PERCENT;
  const resumeThresholdPercent = value?.resumeThresholdPercent ?? DEFAULT_RESUME_THRESHOLD_PERCENT;
  validateApplicationJobQueuePressureThresholds(
    pauseThresholdPercent,
    resumeThresholdPercent,
    (message) => new TypeError(`applicationJobQueue.${message}`)
  );
  return Object.freeze({
    profile,
    maxQueuedApplicationJobs,
    pauseThresholdPercent,
    resumeThresholdPercent
  });
}

function normalizeCoreHwm(
  value: ZLinkFrameworkRegistrationOptions['coreHwm']
): ZLinkFrameworkRegistrationOptions['coreHwm'] {
  if (value === undefined) return undefined;
  const { profile, memoryLimitBytes, budgetBytes } = value;
  if (
    profile !== undefined &&
    (!Number.isInteger(profile) || profile < 0 || profile > AutoHwmProfile.Throughput)
  ) {
    throw new TypeError('coreHwm.profile must be a Core Auto HWM profile value.');
  }
  for (const [name, bytes] of [
    ['memoryLimitBytes', memoryLimitBytes],
    ['budgetBytes', budgetBytes]
  ] as const) {
    if (bytes !== undefined && (typeof bytes !== 'bigint' || bytes <= 0n)) {
      throw new TypeError(`coreHwm.${name} must be a positive bigint.`);
    }
  }
  return Object.freeze({ profile, memoryLimitBytes, budgetBytes });
}

const MAX_APPLICATION_VERSION = 9_223_372_036_854_775_807n;

function normalizeApplicationVersion(value: bigint | undefined): bigint {
  const version = value ?? 0n;
  if (typeof version !== 'bigint' || version < 0n || version > MAX_APPLICATION_VERSION) {
    throw new TypeError(
      'applicationVersion must be a bigint in the signed 64-bit non-negative range.'
    );
  }
  return version;
}

function normalizeMaintenanceWave(value: string | undefined): string | undefined {
  if (value === undefined) return undefined;
  const byteLength = typeof value === 'string' ? new TextEncoder().encode(value).byteLength : 0;
  if (
    typeof value !== 'string' ||
    byteLength === 0 ||
    byteLength > ZLINK_MAX_IDENTITY_TEXT_BYTES ||
    value.includes('\0')
  ) {
    throw new TypeError(
      `maintenanceWave must be a 1..${ZLINK_MAX_IDENTITY_TEXT_BYTES} byte UTF-8 string without NUL.`
    );
  }
  return value;
}

function normalizeNonNegativeInteger(
  value: number | undefined,
  name: string,
  fallback: number
): number {
  if (value === undefined) return fallback;
  if (!Number.isSafeInteger(value) || value < 0) {
    throw new TypeError(`${name} must be a non-negative safe integer.`);
  }
  return value;
}

function normalizePositiveInteger(
  value: number | undefined,
  name: string,
  fallback: number
): number {
  if (value === undefined) return fallback;
  if (!Number.isSafeInteger(value) || value <= 0) {
    throw new TypeError(`${name} must be a positive safe integer.`);
  }
  return value;
}

export function createFrameworkRegistrationWithBuilder(
  configure: (options: ZLinkFrameworkOptions) => void
): ZLinkFrameworkRegistration {
  return createFrameworkRegistration(createFrameworkOptions(configure));
}

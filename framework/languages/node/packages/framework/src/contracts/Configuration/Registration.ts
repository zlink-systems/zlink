import type { ZLinkFrameworkOptions } from '../../contracts';
export { ZLinkConfigurationException } from './ConfigurationException';
import { createFrameworkOptions } from './RegistrationBuilders';
export { createFrameworkOptions } from './RegistrationBuilders';
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
export {
  hasActorManager,
  hasSpotNode,
  hasSpotPublisherClient,
  requirePositiveInteger
} from './RegistrationNormalizers';
import type {
  ZLinkFrameworkRegistration,
  ZLinkFrameworkRegistrationOptions
} from './RegistrationTypes';
import { ZLinkApplicationHwmProfile } from './InboundDispatch';
export * from './RegistrationTypes';
import { validateFrameworkRegistration } from './RegistrationValidators';
export { validateFrameworkRegistration };

const DEFAULT_MESSAGE_FOLLOW_DURATION_MS = 30_000;

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
    requestTimeoutMs: normalizeOptionalPositiveInteger(options.requestTimeoutMs, 'requestTimeoutMs'),
    actorFactories: actorFactoriesFromSpotNodes(spotNodes),
    actorTransferTimeoutMs: normalizeOptionalPositiveInteger(
      options.actorTransferTimeoutMs,
      'actorTransferTimeoutMs'
    ),
    messageFollowDurationMs: normalizeNonNegativeInteger(
      options.messageFollowDurationMs,
      'messageFollowDurationMs',
      DEFAULT_MESSAGE_FOLLOW_DURATION_MS
    ),
    spotFactories: toSpotFactorySet(options.spotFactories, spotNodes),
    channels: toChannelMap(options.channels, network),
    channelClients: channelNamesWith(options.channels, (channel) => channel.client !== undefined),
    fanoutPublishers: channelNamesWith(options.channels, (channel) => channel.publisher !== undefined),
    routeChannels: new Set(routeChannelOptions.keys()),
    routeChannelOptions,
    streamNodes: toStreamNodeMap(options.streamNodes, network),
    streamCompression: normalizeStreamCompression(options.streamCompression),
    spotNodes,
    spotPublisherClients: toSpotPublisherClientSet(options.spotPublisherClients, spotNodes),
    filterTypes: [...(options.filters ?? [])],
    worker: normalizeWorkerOptions(options.worker),
    inboundDispatch: {
      applicationHwmBytes: options.inboundDispatch?.applicationHwmBytes,
      applicationHwmProfile:
        options.inboundDispatch?.applicationHwmProfile
        ?? ZLinkApplicationHwmProfile.Balanced,
      processMemoryLimitBytes:
        options.inboundDispatch?.processMemoryLimitBytes
    },
    dispatch: options.dispatch,
    metrics: options.metrics,
    locations: normalizeLocationRegistration(options.locations)
  };
  validateFrameworkRegistration(registration, options);
  return registration;
}

const MAX_APPLICATION_VERSION = 9_223_372_036_854_775_807n;

function normalizeApplicationVersion(value: bigint | undefined): bigint {
  const version = value ?? 0n;
  if (typeof version !== 'bigint' || version < 0n || version > MAX_APPLICATION_VERSION) {
    throw new TypeError('applicationVersion must be a bigint in the signed 64-bit non-negative range.');
  }
  return version;
}

function normalizeMaintenanceWave(value: string | undefined): string | undefined {
  if (value === undefined) return undefined;
  const byteLength = typeof value === 'string' ? new TextEncoder().encode(value).byteLength : 0;
  if (typeof value !== 'string' || byteLength === 0 || byteLength > 255 || value.includes('\0')) {
    throw new TypeError('maintenanceWave must be a 1..255 byte UTF-8 string without NUL.');
  }
  return value;
}

function normalizeNonNegativeInteger(value: number | undefined, name: string, fallback: number): number {
  if (value === undefined) return fallback;
  if (!Number.isSafeInteger(value) || value < 0) {
    throw new TypeError(`${name} must be a non-negative safe integer.`);
  }
  return value;
}

export function createFrameworkRegistrationWithBuilder(
  configure: (options: ZLinkFrameworkOptions) => void
): ZLinkFrameworkRegistration {
  return createFrameworkRegistration(createFrameworkOptions(configure));
}

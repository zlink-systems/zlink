import type {
  Type,
  ZLinkEntrySpot,
  ZLinkSpot
} from '../../contracts';
import { ZLinkConfigurationException } from './ConfigurationException';

export function validateMessageFollowDuration(timeoutMs: number): number {
  if (!Number.isSafeInteger(timeoutMs) || timeoutMs < 0) {
    throw new ZLinkConfigurationException('Message Follow duration must be a non-negative safe integer.');
  }
  return timeoutMs;
}

export function validateActorTransferTimeout(timeoutMs: number): number {
  if (!Number.isSafeInteger(timeoutMs) || timeoutMs <= 0) {
    throw new ZLinkConfigurationException('actor transfer timeout must be a positive safe integer.');
  }
  return timeoutMs;
}

export function registerEntrySpot(
  options: { entrySpotType?: Type<ZLinkEntrySpot> },
  entrySpotType: Type<ZLinkEntrySpot>
): void {
  if (options.entrySpotType !== undefined) {
    throw new ZLinkConfigurationException('Duplicate Entry Spot registration on SpotNode.');
  }
  options.entrySpotType = entrySpotType;
}

export function registerSpotFactory(
  options: { spotFactories?: Type<ZLinkSpot>[] },
  spotType: Type<ZLinkSpot>
): void {
  options.spotFactories ??= [];
  if (options.spotFactories.includes(spotType)) {
    throw new ZLinkConfigurationException('Duplicate SPOT factory registration on SpotNode.');
  }
  options.spotFactories.push(spotType);
}

export function registerActorFactory(
  options: { actorFactories?: Record<string, Type> },
  actorType: string,
  factoryType: Type
): void {
  const type = actorType.trim();
  if (type.length === 0 || type !== actorType) {
    throw new ZLinkConfigurationException('Actor factory type must not be empty or padded.');
  }
  options.actorFactories ??= {};
  if (Object.hasOwn(options.actorFactories, type)) {
    throw new ZLinkConfigurationException(`Duplicate actor factory '${type}' on SpotNode.`);
  }
  options.actorFactories[type] = factoryType;
}

export function validateRoutingIdPrefix(prefix: string): string {
  if (prefix.trim().length === 0 || prefix !== prefix.trim()) {
    throw new ZLinkConfigurationException('Routing-id prefix must not be empty or padded.');
  }
  if (Buffer.byteLength(`${prefix}-00000000-0000-0000-0000-000000000000`, 'utf8') > 255) {
    throw new ZLinkConfigurationException('Routing-id prefix is too long for a generated routing id.');
  }
  return prefix;
}

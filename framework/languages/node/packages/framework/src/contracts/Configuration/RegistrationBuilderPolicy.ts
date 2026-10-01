const MAX_LISTENER_PORT = 65_535;
export function isValidListenerPort(value: number): boolean {
  return Number.isInteger(value) && value >= 0 && value <= MAX_LISTENER_PORT;
}

export const ZLINK_MAX_CAPACITY = 0x7fff_ffff;
export function isValidPositiveCapacity(value: number): boolean {
  return Number.isSafeInteger(value) && value > 0 && value <= ZLINK_MAX_CAPACITY;
}
export function isValidCapacity(value: number): boolean {
  return Number.isSafeInteger(value) && value >= 0 && value <= ZLINK_MAX_CAPACITY;
}

import { ZLINK_MAX_ROUTING_ID_BYTES } from '../Common/CoreTypes';
import type { Type, ZLinkEntrySpot, ZLinkSpot } from '../../contracts';
import { ZLinkConfigurationException } from './ConfigurationException';

export const ZLINK_DEFAULT_PUBLIC_WEIGHT = 100;
export const ZLINK_MAX_PUBLIC_WEIGHT = 10_000;

export function isValidPublicWeight(value: number): boolean {
  return Number.isInteger(value) && value >= 0 && value <= ZLINK_MAX_PUBLIC_WEIGHT;
}

export function requirePublicWeight(value: number, label: string): number {
  if (!isValidPublicWeight(value)) {
    throw new ZLinkConfigurationException(`${label} must be an integer in 0..10000.`);
  }
  return value;
}

export function validateMessageFollowDuration(timeoutMs: number): number {
  if (!Number.isSafeInteger(timeoutMs) || timeoutMs < 0) {
    throw new ZLinkConfigurationException(
      'Message Follow duration must be a non-negative safe integer.'
    );
  }
  return timeoutMs;
}

export function validateActorTransferTimeout(timeoutMs: number): number {
  if (!Number.isSafeInteger(timeoutMs) || timeoutMs <= 0) {
    throw new ZLinkConfigurationException(
      'actor transfer timeout must be a positive safe integer.'
    );
  }
  return timeoutMs;
}

export function validateSessionReplacementCallbackTimeout(timeoutMs: number): number {
  if (!Number.isSafeInteger(timeoutMs) || timeoutMs <= 0) {
    throw new ZLinkConfigurationException(
      'SessionReplacementCallbackTimeout must be a positive safe integer.'
    );
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
  if (
    Buffer.byteLength(`${prefix}-00000000-0000-0000-0000-000000000000`, 'utf8') >
    ZLINK_MAX_ROUTING_ID_BYTES
  ) {
    throw new ZLinkConfigurationException(
      'Routing-id prefix is too long for a generated routing id.'
    );
  }
  return prefix;
}

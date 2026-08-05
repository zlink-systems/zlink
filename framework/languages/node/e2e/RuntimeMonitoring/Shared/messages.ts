export const RuntimeMonitoringNames = {
  channel: 'monitor.profile',
  throwChannel: 'monitor.throw',
  channelServerSource: 'monitor.profile.server',
  channelClientSource: 'monitor.profile.client',
  locationRuntimeSource: 'monitor.location-runtime',
  spotChannel: 'monitor.spot',
  spotNode: 'monitor.spot',
  publishTopic: 'monitor.profile.events',
  actorType: 'monitor.actor'
} as const;

export const PacketNames = {
  profileReq: 'ProfileReq'
} as const;

export class ProfileReq {
  constructor(
    readonly value: string,
    readonly marker: string
  ) {}
}

export class MonitoringPublish {
  constructor(
    readonly marker: string,
    readonly blocker?: string
  ) {}
}

export interface ProfileRes {
  readonly value: string;
  readonly providerRid: string;
  readonly marker: string;
}

export interface EvidenceWaitReq {
  readonly containsAll: readonly string[];
  readonly containsAnyGroups: readonly (readonly string[])[];
  readonly timeoutMilliseconds?: number;
}

export function socketEventName(event: unknown): string {
  const value = String(event);
  return value.length === 0 ? value : `${value[0].toLowerCase()}${value.slice(1)}`;
}

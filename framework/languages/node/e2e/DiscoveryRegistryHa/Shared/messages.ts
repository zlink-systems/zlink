import { ZLinkPacket } from '@zlink-systems/framework';

export const ChannelNames = {
  profile: 'profile',
  secondary: 'secondary',
  clientServer: 'config6-client-server',
  fanout: 'config6-fanout'
} as const;

export const PacketNames = {
  profileReq: 'ProfileReq',
  secondaryReq: 'SecondaryReq',
  clientServerReq: 'ClientServerReq',
  fanoutEvent: 'FanoutEvent'
} as const;

export class ProfileReq {
  constructor(
    readonly value: string,
    readonly marker?: string
  ) {}
}

export interface ProfileRes {
  readonly value: string;
  readonly providerRid: string;
  readonly marker?: string;
}

export class SecondaryReq {
  constructor(readonly value: string, readonly marker?: string) {}
}

export interface SecondaryRes {
  readonly value: string;
  readonly providerRid: string;
  readonly marker?: string;
}

export class ClientServerReq {
  constructor(readonly value: string, readonly marker?: string) {}
}

export interface ClientServerRes {
  readonly value: string;
  readonly providerRid: string;
  readonly marker?: string;
}

@ZLinkPacket(PacketNames.fanoutEvent)
export class FanoutEvent {
  constructor(readonly value: string, readonly marker?: string) {}
}

export const ObjectSpotType = 'Config6InstanceSpot';

@ZLinkPacket('ObjectReq')
export class ObjectReq {
  constructor(
    readonly spotId: string,
    readonly operationId: string,
    readonly payload: string
  ) {}
}

export interface ObjectRes {
  readonly spotId: string;
  readonly operationId: string;
  readonly payload: string;
  readonly providerRid: string;
}

export interface EvidenceWaitReq {
  readonly contains: string;
  readonly timeoutMilliseconds?: number;
}

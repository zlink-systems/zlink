import { ZLinkPacket } from '@zlink-systems/framework';

export const ChannelNames = {
  profile: 'profile'
} as const;

export const PacketNames = {
  profileReq: 'ProfileReq'
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

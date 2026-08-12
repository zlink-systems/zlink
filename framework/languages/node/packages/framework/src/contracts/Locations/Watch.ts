import type { ZLinkLocationKind } from './Values';

export enum ZLinkLocationChangeScopeKind {
  MeshNode = 'meshNode',
  ClientServer = 'clientServer',
  Spot = 'spot',
  Authority = 'authority',
  OwnerLease = 'ownerLease',
  FanoutPublisher = 'fanoutPublisher'
}

export interface ZLinkLocationChangeStampScope {
  readonly kind: ZLinkLocationKind | ZLinkLocationChangeScopeKind;
  readonly meshName?: string;
  readonly channelName?: string;
}

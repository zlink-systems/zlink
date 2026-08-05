import type { ZLinkLocationOptionOverrides } from '../../contracts/Locations/Options';
import type { ZLinkDomainLocationStore as ZLinkLocationStore } from '../locations/domain-store-contract';
import type {
  ZLinkLocationEventSink,
  ZLinkLocationRuntimeStores
} from '../locations';
import {
  ZLinkLocationRuntime,
  ZLinkOwnerLeaseTracker,
  ZLinkStoreLocationResolvers
} from '../locations';
import type { ZLinkFrameworkRegistration } from '../configuration';
import { ZLinkChannelSocketRegistry } from './channel-socket-registry';

export interface ZLinkChannelLocationAutoConnectContext {
  readonly runtime: ZLinkLocationRuntime;
  readonly stores: ZLinkLocationRuntimeStores;
  readonly options: ZLinkLocationOptionOverrides;
  readonly leaseTracker: ZLinkOwnerLeaseTracker;
  readonly resolver: ZLinkStoreLocationResolvers;
  readonly events?: ZLinkLocationEventSink;
  readonly changeStampStore?: Pick<ZLinkLocationStore, 'getMeshNodeChangeStamp'>;
}

export function createChannelLocationAutoConnectContext(
  runtime: ZLinkLocationRuntime,
  stores: ZLinkLocationRuntimeStores,
  options: ZLinkLocationOptionOverrides,
  events?: ZLinkLocationEventSink
): ZLinkChannelLocationAutoConnectContext {
  const leaseTracker = new ZLinkOwnerLeaseTracker({
    store: stores.ownerLeaseStore,
    options
  });
  return {
    runtime,
    stores,
    options,
    leaseTracker,
    resolver: new ZLinkStoreLocationResolvers({
      stores,
      leaseTracker,
      events
    }),
    events,
    changeStampStore: stores.locationStore
  };
}

export function buildChannelAutoConnectCapabilities(
  _registration: ZLinkFrameworkRegistration,
  _sockets: ZLinkChannelSocketRegistry
): readonly [] {
  return [];
}

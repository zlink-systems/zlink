import { randomUUID } from 'node:crypto';
import {
  ZLinkFrameworkRuntimeState,
  ZLinkLocationWriteIntent,
  ZLinkLocationWriteStatus,
  type ZLinkFanoutPublisherDescriptor,
  type ZLinkLocationOwnerToken
} from '../../contracts/Locations';
import type { ZLinkFanoutLocationStore } from '../locations/internal-store-contracts';
import {
  zlinkRuntimeDefaultLocationOptions,
  type ZLinkLocationOptionOverrides
} from '../../contracts/Locations/Options';
import type { ZLinkFrameworkRegistration } from '../configuration';
import { buildAdvertisedEndpoint, ZLinkConfigurationException } from '../configuration';
import type {
  ZLinkLocationRuntime,
  ZLinkLocationRuntimeStores
} from '../locations';
import type { ZLinkBackendSubscriberSocket } from '../backend/contracts';
import { ZLinkChannelSocketRegistry } from './channel-socket-registry';
import { ZLinkStateLane } from '../execution/state-lane';

interface ActiveFanoutTarget {
  descriptor: ZLinkFanoutPublisherDescriptor;
  readonly connectionId: string;
  readonly subscriber: ZLinkBackendSubscriberSocket;
  reconnectEligible: boolean;
  stopReceiver: () => Promise<void>;
  state: 'connecting' | 'ready';
}

export class ZLinkFanoutLocationRuntime {
  private readonly lane = new ZLinkStateLane();
  private readonly options: Required<ZLinkLocationOptionOverrides>;
  private readonly store: ZLinkFanoutLocationStore;
  private readonly localDescriptors =
    new Map<string, ZLinkFanoutPublisherDescriptor>();
  private readonly publisherIdentities = new Map<string, {
    readonly publisherRid: string;
    readonly lifecycleGeneration: bigint;
  }>();
  private readonly connections = new Map<string, ActiveFanoutTarget>();
  private controller?: AbortController;
  private timer?: NodeJS.Timeout;

  constructor(
    private readonly registration: ZLinkFrameworkRegistration,
    private readonly sockets: ZLinkChannelSocketRegistry,
    private readonly locationRuntime: ZLinkLocationRuntime,
    private readonly stores: ZLinkLocationRuntimeStores,
    options: ZLinkLocationOptionOverrides,
    private readonly onSubscriberOpened: (
      channelName: string,
      connectionId: string,
      subscriber: ZLinkBackendSubscriberSocket
    ) => () => Promise<void>
  ) {
    if (stores.fanoutStore === undefined) {
      throw new ZLinkConfigurationException(
        'Automatic fanout requires a location store with dedicated fanout publisher operations.'
      );
    }
    this.store = stores.fanoutStore;
    this.options = { ...zlinkRuntimeDefaultLocationOptions, ...options };
  }

  async start(signal?: AbortSignal): Promise<void> {
    if (await this.lane.run(() => this.controller !== undefined)) return;
    await this.publishLocalPublishers(signal);
    await this.reconcileSubscribers(signal);
    const started = await this.lane.run(() => {
      if (this.controller !== undefined) return false;
      this.controller = new AbortController();
      return true;
    });
    if (!started) return;
    this.schedule();
  }

  async tick(signal?: AbortSignal): Promise<void> {
    await this.tickCore(signal);
  }

  private async tickCore(signal?: AbortSignal): Promise<void> {
    await this.publishLocalPublishers(signal);
    await this.reconcileSubscribers(signal);
  }

  async stop(signal?: AbortSignal): Promise<void> {
    const timer = await this.lane.run(() => {
      this.controller?.abort();
      this.controller = undefined;
      const current = this.timer;
      this.timer = undefined;
      return current;
    });
    if (timer !== undefined) clearTimeout(timer);
    const connectionIds = await this.lane.run(() => [...this.connections.keys()]);
    await Promise.allSettled(
      connectionIds.map(id => this.closeConnection(id))
    );
    await this.removeLocalPublishers(signal);
  }

  activeTargets(channelName: string): readonly ZLinkFanoutPublisherDescriptor[] {
    return [...this.connections.values()]
      .filter(target => target.state === 'ready')
      .map(target => target.descriptor)
      .filter(descriptor => descriptor.channelName === channelName);
  }

  async reclaimOwnerRows(signal?: AbortSignal): Promise<void> {
    const owner = this.requireOwnerToken();
    const descriptors = await this.lane.run(() => [...this.localDescriptors]);
    for (const [channelName, current] of descriptors) {
      if (current.ownerId === owner.ownerId
        && current.leaseGeneration === owner.leaseGeneration) {
        continue;
      }
      const candidate = {
        ...current,
        ownerId: owner.ownerId,
        leaseGeneration: owner.leaseGeneration
      };
      const result = await this.store.updateFanoutPublisher(
        candidate,
        ZLinkLocationWriteIntent.Takeover,
        signal
      );
      if (result.status !== ZLinkLocationWriteStatus.Stored) {
        throw new ZLinkConfigurationException(
          `Fanout publisher '${channelName}' descriptor recovery was fenced.`
        );
      }
      await this.lane.run(() => this.localDescriptors.set(channelName, {
        ...candidate,
        updatedAt: result.updatedAt
      }));
    }
  }

  private async publishLocalPublishers(signal?: AbortSignal): Promise<void> {
    const owner = this.requireOwnerToken();
    for (const [channelName, channel] of this.registration.channels) {
      if (channel.publisher === undefined) continue;
      const publisher = this.sockets.publisher(channelName);
      const endpoint = advertisedEndpoint(
        publisher.lastEndpoint ?? channel.publisher.bind ?? '',
        channel.publisher.advertiseHost
      );
      if (endpoint.length === 0) {
        throw new ZLinkConfigurationException(
          `Fanout publisher '${channelName}' did not report a bound endpoint.`
        );
      }
      const current = await this.lane.run(() => this.localDescriptors.get(channelName));
      if (current !== undefined) {
        const result = await this.store.updateFanoutPublisher(
          current,
          ZLinkLocationWriteIntent.Renew,
          signal
        );
        if (result.status !== ZLinkLocationWriteStatus.Stored) {
          throw new ZLinkConfigurationException(
            `Fanout publisher '${channelName}' descriptor renewal was fenced.`
          );
        }
        await this.lane.run(() => this.localDescriptors.set(channelName, {
          ...current,
          updatedAt: result.updatedAt
        }));
        continue;
      }
      const generatedLifecycle = BigInt(
        `0x${randomUUID().replaceAll('-', '').slice(0, 16)}`
      ) & 0x7fff_ffff_ffff_ffffn;
      const identity = await this.lane.run(() => {
        const currentIdentity = this.publisherIdentities.get(channelName);
        if (currentIdentity !== undefined) return currentIdentity;
        const created = {
        publisherRid: channel.routingId
          ?? `${channel.routingIdPrefix ?? 'fanout'}-${randomUUID()}`,
        lifecycleGeneration: generatedLifecycle === 0n
          ? 1n
          : generatedLifecycle
        };
        this.publisherIdentities.set(channelName, created);
        return created;
      });
      const descriptor: ZLinkFanoutPublisherDescriptor = {
        channelName,
        publisherRid: identity.publisherRid,
        lifecycleGeneration: identity.lifecycleGeneration,
        descriptorRevision: 1n,
        endpoint,
        state: ZLinkFrameworkRuntimeState.Serving,
        securityIdentity: 'default',
        ownerId: owner.ownerId,
        leaseGeneration: owner.leaseGeneration,
        updatedAt: new Date(0)
      };
      const result = await this.store.updateFanoutPublisher(
        descriptor,
        ZLinkLocationWriteIntent.NewClaim,
        signal
      );
      if (result.status !== ZLinkLocationWriteStatus.Stored) {
        throw new ZLinkConfigurationException(
          `Fanout publisher '${channelName}' descriptor claim failed with '${result.status}'.`
        );
      }
      await this.lane.run(() => this.localDescriptors.set(channelName, {
        ...descriptor,
        updatedAt: result.updatedAt
      }));
    }
  }

  private async reconcileSubscribers(signal?: AbortSignal): Promise<void> {
    for (const [channelName, channel] of this.registration.channels) {
      if (channel.subscriber === undefined
        || (channel.subscriber.manualConnections?.length ?? 0) > 0) {
        continue;
      }
      const rows = await this.listLivePublishers(channelName, signal);
      const desired = new Map(rows.map(row => [
        fanoutConnectionId(row),
        row
      ]));
      for (const [connectionId, descriptor] of desired) {
        const current = await this.lane.run(() => this.connections.get(connectionId));
        if (current === undefined) {
          await this.openConnection(connectionId, descriptor);
          continue;
        }
        if (descriptor.descriptorRevision
          < current.descriptor.descriptorRevision) {
          continue;
        }
        if (descriptor.descriptorRevision
          === current.descriptor.descriptorRevision) {
          if (!sameFanoutDescriptor(descriptor, current.descriptor)) {
            await this.closeConnection(connectionId);
          }
          continue;
        }
        if (!sameFanoutImmutableIdentity(descriptor, current.descriptor)) {
          await this.closeConnection(connectionId);
          continue;
        }
        await this.lane.run(() => {
          if (this.connections.get(connectionId) === current) current.descriptor = descriptor;
        });
        if (current.state === 'ready') {
          this.sockets.admitFanoutPublisher(descriptor, connectionId);
        }
      }
      const connections = await this.lane.run(() => [...this.connections]);
      for (const [connectionId, current] of connections) {
        if (current.descriptor.channelName === channelName
          && !desired.has(connectionId)) {
          await this.closeConnection(connectionId);
        }
      }
    }
  }

  private async openConnection(
    connectionId: string,
    descriptor: ZLinkFanoutPublisherDescriptor
  ): Promise<void> {
    let target: ActiveFanoutTarget | undefined;
    const subscriber = this.sockets.openFanoutSubscriberConnection(
      descriptor.channelName,
      connectionId,
      descriptor.endpoint,
      {
        onReady: () => {
          const current = this.connections.get(connectionId);
          if (target !== undefined && current === target) {
            current.state = 'ready';
            this.sockets.admitFanoutPublisher(descriptor, connectionId);
          }
        },
        onTerminated: () => {
          const current = this.connections.get(connectionId);
          if (target !== undefined
            && current === target
            && current.reconnectEligible) {
            this.sockets.removeFanoutPublisher(descriptor, connectionId);
            current.state = 'connecting';
            setImmediate(() => {
              void this.replaceConnection(current)
                .catch(error =>
                  this.locationRuntime.reportDiscoveryFailure(error));
            });
          }
        }
      }
    );
    target = {
      descriptor,
      connectionId,
      subscriber,
      reconnectEligible: true,
      stopReceiver: async () => {},
      state: 'connecting'
    };
    await this.lane.run(() => this.connections.set(connectionId, target));
    target.stopReceiver = this.onSubscriberOpened(
      descriptor.channelName,
      connectionId,
      subscriber
    );
  }

  private async closeConnection(connectionId: string): Promise<void> {
    const current = await this.lane.run(() => {
      const target = this.connections.get(connectionId);
      if (target !== undefined) {
        target.reconnectEligible = false;
        this.connections.delete(connectionId);
      }
      return target;
    });
    if (current === undefined) return;
    this.sockets.removeFanoutPublisher(current.descriptor, connectionId);
    await current.stopReceiver();
    await this.sockets.closeFanoutSubscriberConnection(connectionId);
  }

  private async replaceConnection(
    expected: ActiveFanoutTarget
  ): Promise<void> {
    const prepared = await this.lane.run(() => {
      const controller = this.controller;
      if (this.connections.get(expected.connectionId) !== expected
        || !expected.reconnectEligible
        || controller === undefined) return undefined;
      return { controller, descriptor: expected.descriptor };
    });
    if (prepared === undefined) {
      return;
    }
    await this.closeConnection(expected.connectionId);
    if (await this.lane.run(() => this.controller === prepared.controller)) {
      await this.openConnection(expected.connectionId, prepared.descriptor);
    }
  }

  private async listLivePublishers(
    channelName: string,
    signal?: AbortSignal
  ): Promise<ZLinkFanoutPublisherDescriptor[]> {
    const rows: ZLinkFanoutPublisherDescriptor[] = [];
    let continuationToken: string | undefined;
    do {
      const page = await this.store.listFanoutPublishers(
        channelName,
        { pageSize: 1000, continuationToken },
        signal
      );
      rows.push(...page.items);
      continuationToken = page.continuationToken;
    } while (continuationToken !== undefined);
    const live: ZLinkFanoutPublisherDescriptor[] = [];
    for (const descriptor of rows) {
      if (descriptor.state !== ZLinkFrameworkRuntimeState.Serving) continue;
      const lease = await this.storeOwnerLease(descriptor.ownerId, signal);
      if (lease.kind === 'found'
        && lease.token.leaseGeneration === descriptor.leaseGeneration
        && lease.leaseExpiresAt.getTime() > lease.storeNow.getTime()) {
        live.push(descriptor);
      }
    }
    return live;
  }

  private storeOwnerLease(ownerId: string, signal?: AbortSignal) {
    return this.stores.ownerLeaseStore.readOwnerLease(
      ownerId,
      signal
    );
  }

  private async removeLocalPublishers(signal?: AbortSignal): Promise<void> {
    const owner = this.locationRuntime.currentOwnerToken;
    if (owner === undefined) return;
    const descriptors = await this.lane.run(() => [...this.localDescriptors.values()]);
    for (const descriptor of descriptors) {
      await this.store.removeFanoutPublisher({
        channelName: descriptor.channelName,
        publisherRid: descriptor.publisherRid
      }, owner, signal);
    }
    await this.lane.run(() => this.localDescriptors.clear());
  }

  private requireOwnerToken(): ZLinkLocationOwnerToken {
    const token = this.locationRuntime.currentOwnerToken;
    if (token === undefined) {
      throw new ZLinkConfigurationException(
        'Fanout descriptor publication requires an active owner lease.'
      );
    }
    return token;
  }

  private schedule(): void {
    if (this.controller === undefined) return;
    this.timer = setTimeout(() => {
      this.timer = undefined;
      void this.tickCore(this.controller?.signal)
        .catch(error => this.locationRuntime.reportDiscoveryFailure(error))
        .finally(() => this.schedule());
    }, this.options.pollingIntervalMs);
    this.timer.unref();
  }
}

function advertisedEndpoint(boundEndpoint: string, advertiseHost: string | undefined): string {
  const result = buildAdvertisedEndpoint(boundEndpoint, advertiseHost);
  if (result === undefined) {
    throw new ZLinkConfigurationException(
      `Fanout publisher bound endpoint '${boundEndpoint}' cannot be advertised.`
    );
  }
  return result;
}

function fanoutConnectionId(
  descriptor: ZLinkFanoutPublisherDescriptor
): string {
  return `${descriptor.channelName}\0${String(descriptor.publisherRid)}\0`
    + descriptor.lifecycleGeneration.toString();
}

function sameFanoutImmutableIdentity(
  left: ZLinkFanoutPublisherDescriptor,
  right: ZLinkFanoutPublisherDescriptor
): boolean {
  return left.channelName === right.channelName
    && left.publisherRid === right.publisherRid
    && left.lifecycleGeneration === right.lifecycleGeneration
    && left.endpoint === right.endpoint
    && left.securityIdentity === right.securityIdentity
    && left.ownerId === right.ownerId
    && left.leaseGeneration === right.leaseGeneration;
}

function sameFanoutDescriptor(
  left: ZLinkFanoutPublisherDescriptor,
  right: ZLinkFanoutPublisherDescriptor
): boolean {
  return sameFanoutImmutableIdentity(left, right)
    && left.descriptorRevision === right.descriptorRevision
    && left.state === right.state;
}

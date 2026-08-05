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
import { ZLinkConfigurationException } from '../configuration';
import type {
  ZLinkLocationRuntime,
  ZLinkLocationRuntimeStores
} from '../locations';
import type { ZLinkBackendSubscriberSocket } from '../backend/contracts';
import { ZLinkChannelSocketRegistry } from './channel-socket-registry';

interface ActiveFanoutTarget {
  descriptor: ZLinkFanoutPublisherDescriptor;
  readonly connectionId: string;
  readonly subscriber: ZLinkBackendSubscriberSocket;
  reconnectEligible: boolean;
  stopReceiver: () => Promise<void>;
  state: 'connecting' | 'ready';
}

export class ZLinkFanoutLocationRuntime {
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
    if (this.controller !== undefined) return;
    await this.publishLocalPublishers(signal);
    await this.reconcileSubscribers(signal);
    this.controller = new AbortController();
    this.schedule();
  }

  async tick(signal?: AbortSignal): Promise<void> {
    await this.publishLocalPublishers(signal);
    await this.reconcileSubscribers(signal);
  }

  async stop(signal?: AbortSignal): Promise<void> {
    this.controller?.abort();
    this.controller = undefined;
    if (this.timer !== undefined) {
      clearTimeout(this.timer);
      this.timer = undefined;
    }
    await Promise.allSettled(
      [...this.connections.keys()].map(id => this.closeConnection(id))
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
    for (const [channelName, current] of this.localDescriptors) {
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
      this.localDescriptors.set(channelName, {
        ...candidate,
        updatedAt: result.updatedAt
      });
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
      const current = this.localDescriptors.get(channelName);
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
        this.localDescriptors.set(channelName, {
          ...current,
          updatedAt: result.updatedAt
        });
        continue;
      }
      const generatedLifecycle = BigInt(
        `0x${randomUUID().replaceAll('-', '').slice(0, 16)}`
      ) & 0x7fff_ffff_ffff_ffffn;
      const identity = this.publisherIdentities.get(channelName) ?? {
        publisherRid: channel.routingId
          ?? `${channel.routingIdPrefix ?? 'fanout'}-${randomUUID()}`,
        lifecycleGeneration: generatedLifecycle === 0n
          ? 1n
          : generatedLifecycle
      };
      this.publisherIdentities.set(channelName, identity);
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
      this.localDescriptors.set(channelName, {
        ...descriptor,
        updatedAt: result.updatedAt
      });
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
        const current = this.connections.get(connectionId);
        if (current === undefined) {
          this.openConnection(connectionId, descriptor);
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
        current.descriptor = descriptor;
        if (current.state === 'ready') {
          this.sockets.admitFanoutPublisher(descriptor, connectionId);
        }
      }
      for (const [connectionId, current] of [...this.connections]) {
        if (current.descriptor.channelName === channelName
          && !desired.has(connectionId)) {
          await this.closeConnection(connectionId);
        }
      }
    }
  }

  private openConnection(
    connectionId: string,
    descriptor: ZLinkFanoutPublisherDescriptor
  ): void {
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
    this.connections.set(connectionId, target);
    target.stopReceiver = this.onSubscriberOpened(
      descriptor.channelName,
      connectionId,
      subscriber
    );
  }

  private async closeConnection(connectionId: string): Promise<void> {
    const current = this.connections.get(connectionId);
    if (current === undefined) return;
    current.reconnectEligible = false;
    this.connections.delete(connectionId);
    this.sockets.removeFanoutPublisher(current.descriptor, connectionId);
    await current.stopReceiver();
    await this.sockets.closeFanoutSubscriberConnection(connectionId);
  }

  private async replaceConnection(
    expected: ActiveFanoutTarget
  ): Promise<void> {
    const controller = this.controller;
    if (this.connections.get(expected.connectionId) !== expected
      || !expected.reconnectEligible
      || controller === undefined) {
      return;
    }
    const descriptor = expected.descriptor;
    await this.closeConnection(expected.connectionId);
    if (this.controller === controller) {
      this.openConnection(expected.connectionId, descriptor);
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
    for (const descriptor of this.localDescriptors.values()) {
      await this.store.removeFanoutPublisher({
        channelName: descriptor.channelName,
        publisherRid: descriptor.publisherRid
      }, owner, signal);
    }
    this.localDescriptors.clear();
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
      void this.tick(this.controller?.signal)
        .catch(error => this.locationRuntime.reportDiscoveryFailure(error))
        .finally(() => this.schedule());
    }, this.options.pollingIntervalMs);
    this.timer.unref();
  }
}

function advertisedEndpoint(boundEndpoint: string, advertiseHost: string | undefined): string {
  if (advertiseHost === undefined) return boundEndpoint;
  const match = /^([a-z][a-z0-9+.-]*):\/\/(?:\[[^\]]+\]|[^:]+):(\d+)$/i.exec(boundEndpoint);
  if (match === null) {
    throw new ZLinkConfigurationException(
      `Fanout publisher bound endpoint '${boundEndpoint}' cannot be advertised.`
    );
  }
  const host = advertiseHost.includes(':') && !advertiseHost.startsWith('[')
    ? `[${advertiseHost}]`
    : advertiseHost;
  return `${match[1]}://${host}:${match[2]}`;
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

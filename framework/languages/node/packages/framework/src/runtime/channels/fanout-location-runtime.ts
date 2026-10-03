import { randomUUID } from 'node:crypto';
import {
  ZLinkFrameworkRuntimeState,
  ZLinkLocationWriteIntent,
  ZLinkLocationWriteStatus,
  type ZLinkFanoutPublisherDescriptor,
  type ZLinkLocationOwnerToken
} from '../../contracts/Locations';
import {
  zlinkRuntimeDefaultLocationOptions,
  type ZLinkLocationOptionOverrides
} from '../../contracts/Locations/Options';
import { ZLINK_PROVIDER_MAX_PAGE_SIZE } from '../../contracts/Locations/Stores';
import type { ZLinkBackendSubscriberSocket } from '../backend/contracts';
import { type ZLinkFrameworkRegistration, ZLinkConfigurationException } from '../configuration';

import { ZLinkStateLane } from '../execution/state-lane';
import { discoveryAvailabilityForRuntimeState } from '../foundation/runtime-state-projections';
import { closeResources, finishResourceCleanup } from '../foundation/event-loop-resources';
import type { ZLinkLocationRuntime, ZLinkLocationRuntimeStores } from '../locations';
import type { ZLinkFanoutLocationStore } from '../locations/internal-store-contracts';
import { ZLinkChannelSocketRegistry } from './channel-socket-registry';

interface FanoutTarget {
  descriptor: ZLinkFanoutPublisherDescriptor;
  connection?: ActiveFanoutConnection;
}

interface ActiveFanoutConnection {
  readonly connectionId: string;
  stopReceiver: () => Promise<void>;
  state: 'connecting' | 'ready';
}

export class ZLinkFanoutLocationRuntime {
  private readonly lane = new ZLinkStateLane();
  private readonly options: Required<ZLinkLocationOptionOverrides>;
  private readonly store: ZLinkFanoutLocationStore;
  private readonly localDescriptors = new Map<string, ZLinkFanoutPublisherDescriptor>();
  private readonly publisherIdentities = new Map<
    string,
    {
      readonly publisherRid: string;
      readonly lifecycleGeneration: bigint;
    }
  >();
  private readonly targets = new Map<string, FanoutTarget>();
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
    const targets = await this.lane.run(() => [...this.targets]);
    const targetResults = await Promise.allSettled(
      targets.map(([id, target]) => this.removeTarget(id, target))
    );
    const publisherResults = await Promise.allSettled([this.removeLocalPublishers(signal)]);
    finishResourceCleanup(
      [...targetResults, ...publisherResults].flatMap((result) =>
        result.status === 'rejected' ? [result.reason] : []
      )
    );
  }

  activeTargets(channelName: string): readonly ZLinkFanoutPublisherDescriptor[] {
    return [...this.targets.values()]
      .filter((target) => target.connection?.state === 'ready')
      .map((target) => target.descriptor)
      .filter((descriptor) => descriptor.channelName === channelName);
  }

  topologyTargets(channelName: string) {
    if (!this.isAutomaticSubscriber(channelName)) return undefined;
    return [...this.targets.values()]
      .filter((target) => target.descriptor.channelName === channelName)
      .map((target) => ({
        channelName,
        publisherRoutingId: String(target.descriptor.publisherRid),
        lifecycleGeneration: target.descriptor.lifecycleGeneration,
        descriptorRevision: target.descriptor.descriptorRevision,
        advertisedEndpoint: target.descriptor.endpoint,
        state:
          target.descriptor.state === ZLinkFrameworkRuntimeState.Serving
            ? target.connection?.state === 'ready'
              ? ('serving' as const)
              : ('preparing' as const)
            : discoveryAvailabilityForRuntimeState(target.descriptor.state)
      }));
  }

  private isAutomaticSubscriber(channelName: string): boolean {
    const subscriber = this.registration.channels.get(channelName)?.subscriber;
    return subscriber !== undefined && (subscriber.manualConnections?.length ?? 0) === 0;
  }

  async reclaimOwnerRows(signal?: AbortSignal): Promise<void> {
    const owner = this.requireOwnerToken();
    const descriptors = await this.lane.run(() => [...this.localDescriptors]);
    for (const [channelName, current] of descriptors) {
      if (current.ownerId === owner.ownerId && current.leaseGeneration === owner.leaseGeneration) {
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
      await this.lane.run(() =>
        this.localDescriptors.set(channelName, {
          ...candidate,
          updatedAt: result.updatedAt
        })
      );
    }
  }

  private async publishLocalPublishers(signal?: AbortSignal): Promise<void> {
    const owner = this.requireOwnerToken();
    for (const [channelName, channel] of this.registration.channels) {
      if (channel.publisher === undefined) continue;
      // Opening the publisher binds it and fixes its advertised endpoint.
      this.sockets.publisher(channelName);
      const endpoint = this.sockets.fanoutPublisherEndpoint(channelName) ?? '';
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
        await this.lane.run(() =>
          this.localDescriptors.set(channelName, {
            ...current,
            updatedAt: result.updatedAt
          })
        );
        continue;
      }
      const generatedLifecycle =
        BigInt(`0x${randomUUID().replaceAll('-', '').slice(0, 16)}`) & 0x7fff_ffff_ffff_ffffn;
      const identity = await this.lane.run(() => {
        const currentIdentity = this.publisherIdentities.get(channelName);
        if (currentIdentity !== undefined) return currentIdentity;
        const created = {
          publisherRid:
            channel.routingId ?? `${channel.routingIdPrefix ?? 'fanout'}-${randomUUID()}`,
          lifecycleGeneration: generatedLifecycle === 0n ? 1n : generatedLifecycle
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
      await this.lane.run(() =>
        this.localDescriptors.set(channelName, {
          ...descriptor,
          updatedAt: result.updatedAt
        })
      );
    }
  }

  private async reconcileSubscribers(signal?: AbortSignal): Promise<void> {
    for (const channelName of this.registration.channels.keys()) {
      if (!this.isAutomaticSubscriber(channelName)) continue;
      const previous = this.topologyTargets(channelName)!;
      const rows = await this.listLivePublishers(channelName, signal);
      const desired = new Map(rows.map((row) => [fanoutConnectionId(row), row]));
      for (const [connectionId, descriptor] of desired) {
        let current = await this.lane.run(() => this.targets.get(connectionId));
        if (current === undefined) {
          const created = { descriptor };
          current = created;
          await this.lane.run(() => this.targets.set(connectionId, created));
        } else if (descriptor.descriptorRevision < current.descriptor.descriptorRevision) {
          continue;
        } else if (descriptor.descriptorRevision === current.descriptor.descriptorRevision) {
          if (!sameFanoutDescriptor(descriptor, current.descriptor)) {
            await this.removeTarget(connectionId, current);
            continue;
          }
        } else if (!sameFanoutImmutableIdentity(descriptor, current.descriptor)) {
          await this.removeTarget(connectionId, current);
          continue;
        } else {
          const target = current;
          await this.lane.run(() => {
            if (this.targets.get(connectionId) === target) target.descriptor = descriptor;
          });
          if (current.connection?.state === 'ready') {
            this.sockets.admitFanoutPublisher(current.descriptor, connectionId);
          }
        }
        await this.reconcileConnection(connectionId, current);
      }
      const targets = await this.lane.run(() => [...this.targets]);
      for (const [connectionId, current] of targets) {
        if (current.descriptor.channelName === channelName && !desired.has(connectionId)) {
          await this.removeTarget(connectionId, current);
        }
      }
      const next = this.topologyTargets(channelName)!;
      if (
        previous.length !== next.length ||
        previous.some(
          (publisher, index) =>
            publisher.publisherRoutingId !== next[index]!.publisherRoutingId ||
            publisher.state !== next[index]!.state
        )
      ) {
        this.sockets.notifyFanoutTopology(channelName);
      }
    }
  }

  private async reconcileConnection(connectionId: string, target: FanoutTarget): Promise<void> {
    if (target.descriptor.state !== ZLinkFrameworkRuntimeState.Serving) {
      await this.closeConnection(target);
    } else if (target.connection === undefined) {
      await this.openConnection(connectionId, target);
    }
  }

  private async openConnection(connectionId: string, target: FanoutTarget): Promise<void> {
    const descriptor = target.descriptor;
    let connection: ActiveFanoutConnection | undefined;
    const subscriber = this.sockets.openFanoutSubscriberConnection(
      descriptor.channelName,
      connectionId,
      descriptor.endpoint,
      {
        onReady: () => {
          if (connection !== undefined && target.connection === connection) {
            connection.state = 'ready';
            this.sockets.admitFanoutPublisher(target.descriptor, connectionId);
          }
        },
        onTerminated: () => {
          if (connection !== undefined && target.connection === connection) {
            this.sockets.removeFanoutPublisher(target.descriptor, connectionId);
            connection.state = 'connecting';
            const terminated = connection;
            setImmediate(() => {
              void this.replaceConnection(target, terminated).catch((error) =>
                this.locationRuntime.reportDiscoveryFailure(error)
              );
            });
          }
        }
      }
    );
    connection = {
      connectionId,
      stopReceiver: async () => {},
      state: 'connecting'
    };
    const opened = connection;
    await this.lane.run(() => (target.connection = opened));
    connection.stopReceiver = this.onSubscriberOpened(
      descriptor.channelName,
      connectionId,
      subscriber
    );
  }

  private async removeTarget(connectionId: string, target: FanoutTarget): Promise<void> {
    const detached = await this.lane.run(() => {
      if (this.targets.get(connectionId) !== target) return undefined;
      this.targets.delete(connectionId);
      return this.detachConnection(target);
    });
    await this.closeDetachedConnection(target.descriptor, detached);
  }

  private async closeConnection(target: FanoutTarget): Promise<void> {
    const detached = await this.lane.run(() => this.detachConnection(target));
    await this.closeDetachedConnection(target.descriptor, detached);
  }

  private detachConnection(target: FanoutTarget): ActiveFanoutConnection | undefined {
    const connection = target.connection;
    target.connection = undefined;
    return connection;
  }

  private async closeDetachedConnection(
    descriptor: ZLinkFanoutPublisherDescriptor,
    current: ActiveFanoutConnection | undefined
  ): Promise<void> {
    if (current === undefined) return;
    this.sockets.removeFanoutPublisher(descriptor, current.connectionId);
    await closeResources([
      { close: () => current.stopReceiver() },
      { close: () => this.sockets.closeFanoutSubscriberConnection(current.connectionId) }
    ]);
  }

  private async replaceConnection(
    target: FanoutTarget,
    expected: ActiveFanoutConnection
  ): Promise<void> {
    const prepared = await this.lane.run(() => {
      const controller = this.controller;
      if (
        this.targets.get(expected.connectionId) !== target ||
        target.connection !== expected ||
        controller === undefined
      )
        return undefined;
      return controller;
    });
    if (prepared === undefined) {
      return;
    }
    await this.closeConnection(target);
    if (
      await this.lane.run(
        () => this.controller === prepared && this.targets.get(expected.connectionId) === target
      )
    ) {
      await this.reconcileConnection(expected.connectionId, target);
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
        { pageSize: ZLINK_PROVIDER_MAX_PAGE_SIZE, continuationToken },
        signal
      );
      rows.push(...page.items);
      continuationToken = page.continuationToken;
    } while (continuationToken !== undefined);
    const live: ZLinkFanoutPublisherDescriptor[] = [];
    for (const descriptor of rows) {
      const lease = await this.storeOwnerLease(descriptor.ownerId, signal);
      if (
        lease.kind === 'found' &&
        lease.token.leaseGeneration === descriptor.leaseGeneration &&
        lease.leaseExpiresAt.getTime() > lease.storeNow.getTime()
      ) {
        live.push(descriptor);
      }
    }
    return live;
  }

  private storeOwnerLease(ownerId: string, signal?: AbortSignal) {
    return this.stores.ownerLeaseStore.readOwnerLease(ownerId, signal);
  }

  private async removeLocalPublishers(signal?: AbortSignal): Promise<void> {
    const owner = this.locationRuntime.currentOwnerToken;
    if (owner === undefined) return;
    const descriptors = await this.lane.run(() => [...this.localDescriptors.values()]);
    try {
      await closeResources(
        descriptors.map((descriptor) => ({
          close: async () => {
            await this.store.removeFanoutPublisher(
              {
                channelName: descriptor.channelName,
                publisherRid: descriptor.publisherRid
              },
              owner,
              signal
            );
          }
        }))
      );
    } finally {
      await this.lane.run(() => this.localDescriptors.clear());
    }
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
        .catch((error) => this.locationRuntime.reportDiscoveryFailure(error))
        .finally(() => this.schedule());
    }, this.options.pollingIntervalMs);
    this.timer.unref();
  }
}

function fanoutConnectionId(descriptor: ZLinkFanoutPublisherDescriptor): string {
  return (
    `${descriptor.channelName}\0${String(descriptor.publisherRid)}\0` +
    descriptor.lifecycleGeneration.toString()
  );
}

function sameFanoutImmutableIdentity(
  left: ZLinkFanoutPublisherDescriptor,
  right: ZLinkFanoutPublisherDescriptor
): boolean {
  return (
    left.channelName === right.channelName &&
    left.publisherRid === right.publisherRid &&
    left.lifecycleGeneration === right.lifecycleGeneration &&
    left.endpoint === right.endpoint &&
    left.securityIdentity === right.securityIdentity &&
    left.ownerId === right.ownerId &&
    left.leaseGeneration === right.leaseGeneration
  );
}

function sameFanoutDescriptor(
  left: ZLinkFanoutPublisherDescriptor,
  right: ZLinkFanoutPublisherDescriptor
): boolean {
  return (
    sameFanoutImmutableIdentity(left, right) &&
    left.descriptorRevision === right.descriptorRevision &&
    left.state === right.state
  );
}

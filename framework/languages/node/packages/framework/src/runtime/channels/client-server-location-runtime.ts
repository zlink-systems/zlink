import {
  ZLinkFrameworkRuntimeState,
  ZLinkLocationWriteIntent,
  ZLinkLocationWriteStatus,
  type ZLinkClientServerServerDescriptor,
  type ZLinkLocationOwnerToken
} from '../../contracts/Locations';
import type { ZLinkClientServerLocationStore } from '../locations/internal-store-contracts';
import {
  zlinkRuntimeDefaultLocationOptions,
  type ZLinkLocationOptionOverrides
} from '../../contracts/Locations/Options';
import type { ZLinkFrameworkRegistration } from '../configuration';
import { ZLinkConfigurationException } from '../configuration';
import type { ZLinkLocationRuntime, ZLinkLocationRuntimeStores } from '../locations';
import { ZLinkChannelSocketRegistry } from './channel-socket-registry';
import type { ZLinkBackendDealerSocket } from '../backend/contracts';
import { ZLinkBufferMessage as RuntimeMessage } from '../backend/runtime-message';
import type { Message } from '../../contracts/Common/Message';
import {
  decodeClientServerControl,
  encodeClientServerHello,
  type ZLinkClientServerAdmission
} from './client-server-service-wire';

interface ActiveClientServerTarget {
  descriptor: ZLinkClientServerServerDescriptor;
  readonly connectionId: string;
  dealer?: ZLinkBackendDealerSocket;
  state: 'pending' | 'ready';
  handshakeInFlight: boolean;
}

/**
 * Owns the dedicated ClientServer discovery domain. It never converts these
 * descriptors into RouteMesh peer rows.
 */
export class ZLinkClientServerLocationRuntime {
  private readonly options: Required<ZLinkLocationOptionOverrides>;
  private readonly store: ZLinkClientServerLocationStore;
  private readonly localDescriptors = new Map<string, ZLinkClientServerServerDescriptor>();
  private readonly connections = new Map<string, ActiveClientServerTarget>();
  private controller?: AbortController;
  private timer?: NodeJS.Timeout;

  constructor(
    private readonly registration: ZLinkFrameworkRegistration,
    private readonly sockets: ZLinkChannelSocketRegistry,
    private readonly locationRuntime: ZLinkLocationRuntime,
    private readonly stores: ZLinkLocationRuntimeStores,
    options: ZLinkLocationOptionOverrides
  ) {
    if (stores.clientServerStore === undefined) {
      throw new ZLinkConfigurationException(
        'ClientServer automatic discovery requires a location store with dedicated ClientServer descriptor operations.'
      );
    }
    this.store = stores.clientServerStore;
    this.options = { ...zlinkRuntimeDefaultLocationOptions, ...options };
  }

  async start(signal?: AbortSignal): Promise<void> {
    if (this.controller !== undefined) return;
    await this.publishServers(signal);
    await this.reconcileClients(signal);
    this.controller = new AbortController();
    this.schedule();
  }

  async tick(signal?: AbortSignal): Promise<void> {
    await this.publishServers(signal);
    await this.reconcileClients(signal);
  }

  async stop(signal?: AbortSignal): Promise<void> {
    this.controller?.abort();
    this.controller = undefined;
    if (this.timer !== undefined) {
      clearTimeout(this.timer);
      this.timer = undefined;
    }
    await this.disconnectClients();
    await this.drainAndRemoveServers(signal);
  }

  activeTargets(channelName: string): readonly ZLinkClientServerServerDescriptor[] {
    return [...this.connections.values()]
      .filter(target => target.state === 'ready')
      .map((target) => target.descriptor)
      .filter((descriptor) => descriptor.channelName === channelName);
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
      const result = await this.store.updateClientServer(
        candidate,
        ZLinkLocationWriteIntent.Takeover,
        signal
      );
      if (result.status !== ZLinkLocationWriteStatus.Stored) {
        throw new ZLinkConfigurationException(
          `ClientServer server '${channelName}' descriptor recovery was fenced.`
        );
      }
      const published = { ...candidate, updatedAt: result.updatedAt };
      this.localDescriptors.set(channelName, published);
      this.sockets.setClientServerServerDescriptor(published, channelName);
    }
  }

  private async publishServers(signal?: AbortSignal): Promise<void> {
    const owner = this.requireOwnerToken();
    for (const [channelName, channel] of this.registration.channels) {
      if (channel.server === undefined) continue;
      const current = this.localDescriptors.get(channelName);
      if (current !== undefined) {
        const runtimeWeight = this.sockets.clientServerServerWeight(channelName);
        const candidate = runtimeWeight === current.weight
          ? current
          : {
              ...current,
              descriptorRevision: current.descriptorRevision + 1n,
              weight: runtimeWeight
            };
        const result = await this.store.updateClientServer(
          candidate,
          ZLinkLocationWriteIntent.Renew,
          signal
        );
        if (result.status !== ZLinkLocationWriteStatus.Stored) {
          throw new ZLinkConfigurationException(
            `ClientServer server '${channelName}' descriptor renewal was fenced.`
          );
        }
        const published = { ...candidate, updatedAt: result.updatedAt };
        this.localDescriptors.set(channelName, published);
        this.sockets.setClientServerServerDescriptor(published, channelName);
        continue;
      }
      const identity = this.sockets.clientServerServerIdentity(channelName);
      const descriptor: ZLinkClientServerServerDescriptor = {
        channelName,
        serverRid: identity.serverRid,
        lifecycleGeneration: identity.lifecycleGeneration,
        descriptorRevision: 1n,
        endpoint: identity.endpoint,
        weight: channel.server.weight ?? 100,
        state: ZLinkFrameworkRuntimeState.Serving,
        securityIdentity: 'default',
        ownerId: owner.ownerId,
        leaseGeneration: owner.leaseGeneration,
        updatedAt: new Date(0)
      };
      const result = await this.store.updateClientServer(
        descriptor,
        ZLinkLocationWriteIntent.NewClaim,
        signal
      );
      if (result.status !== ZLinkLocationWriteStatus.Stored) {
        throw new ZLinkConfigurationException(
          `ClientServer server '${channelName}' descriptor claim failed with '${result.status}'.`
        );
      }
      const published = { ...descriptor, updatedAt: result.updatedAt };
      this.localDescriptors.set(channelName, published);
      this.sockets.setClientServerServerDescriptor(published, channelName);
    }
  }

  private async reconcileClients(signal?: AbortSignal): Promise<void> {
    for (const [channelName, channel] of this.registration.channels) {
      if (channel.client === undefined || (channel.client.manualConnections?.length ?? 0) > 0) {
        continue;
      }
      const rows = await this.listLiveServers(channelName, signal);
      const desired = new Map(rows.map((descriptor) => [
        clientServerConnectionId(descriptor),
        descriptor
      ]));
      const desiredStableKeys = new Set(rows.map(clientServerStableKey));
      for (const [connectionId, descriptor] of desired) {
        const current = this.connections.get(connectionId);
        if (current === undefined) {
          this.openConnection(descriptor, connectionId);
          continue;
        }
        if (!sameDescriptor(current.descriptor, descriptor)) {
          if (current.state === 'ready') {
            const updated = this.sockets.admitClientServerConnection(
              toDiscoveryDescriptor(descriptor),
              connectionId
            );
            if (updated) current.descriptor = descriptor;
          } else {
            current.descriptor = descriptor;
          }
        }
      }
      for (const [connectionId, current] of [...this.connections]) {
        if (current.descriptor.channelName !== channelName || desired.has(connectionId)) continue;
        if (desiredStableKeys.has(clientServerStableKey(current.descriptor))
          && current.state === 'ready') {
          continue;
        }
        await this.closeConnection(connectionId);
      }
    }
  }

  private async listLiveServers(
    channelName: string,
    signal?: AbortSignal
  ): Promise<ZLinkClientServerServerDescriptor[]> {
    const rows: ZLinkClientServerServerDescriptor[] = [];
    let continuationToken: string | undefined;
    do {
      const page = await this.store.listClientServers(
        channelName,
        { pageSize: 1000, continuationToken },
        signal
      );
      rows.push(...page.items);
      continuationToken = page.continuationToken;
    } while (continuationToken !== undefined);

    const live: ZLinkClientServerServerDescriptor[] = [];
    for (const descriptor of rows) {
      if (descriptor.state === ZLinkFrameworkRuntimeState.Stopped
        || descriptor.state === ZLinkFrameworkRuntimeState.Error) {
        continue;
      }
      const lease = await this.stores.ownerLeaseStore.readOwnerLease(
        descriptor.ownerId,
        signal
      );
      if (lease.kind === 'found'
        && lease.token.leaseGeneration === descriptor.leaseGeneration
        && lease.leaseExpiresAt.getTime() > lease.storeNow.getTime()) {
        live.push(descriptor);
      }
    }
    return live;
  }

  private async disconnectClients(): Promise<void> {
    await Promise.allSettled(
      [...this.connections.keys()].map(connectionId => this.closeConnection(connectionId))
    );
  }

  private openConnection(
    descriptor: ZLinkClientServerServerDescriptor,
    connectionId: string
  ): void {
    const target: ActiveClientServerTarget = {
      descriptor,
      connectionId,
      state: 'pending',
      handshakeInFlight: false
    };
    this.connections.set(connectionId, target);
    try {
      target.dealer = this.sockets.openClientServerConnection(
        descriptor.channelName,
        connectionId,
        descriptor.endpoint,
        {
          onTransportReady: (routingId, endpoint) => {
            void this.handleTransportReady(connectionId, routingId, endpoint)
              .catch(error => {
                this.locationRuntime.reportDiscoveryFailure(error);
                void this.closeConnection(connectionId);
              });
          },
          onTerminated: () => {
            void this.handleConnectionTerminated(connectionId);
          }
        }
      );
    } catch (error) {
      this.connections.delete(connectionId);
      throw error;
    }
  }

  private async handleTransportReady(
    connectionId: string,
    _routingId: string,
    _endpoint: string
  ): Promise<void> {
    const current = this.connections.get(connectionId);
    if (current === undefined
      || current.state === 'ready'
      || current.handshakeInFlight) {
      return;
    }
    const dealer = current.dealer;
    if (dealer === undefined) return;
    current.handshakeInFlight = true;
    try {
      const admission = await requestAdmission(
        dealer,
        current.descriptor,
        this.options.ownerLeaseRenewTimeoutMs
      );
      const latest = this.connections.get(connectionId);
      if (latest !== current) return;
      requireMatchingAdmission(current.descriptor, admission);
      const admittedDescriptor = {
        ...current.descriptor,
        descriptorRevision: admission.descriptorRevision,
        weight: admission.weight,
        state: admission.state
      };
      current.descriptor = admittedDescriptor;
      if (!this.sockets.admitClientServerConnection(
        toDiscoveryDescriptor(admittedDescriptor, admission.normalizedEffectiveMaxMessageBytes),
        connectionId
      )) {
        throw new ZLinkConfigurationException(
          `ClientServer '${current.descriptor.channelName}' admission was stale.`
        );
      }
      current.state = 'ready';
      await this.removeSupersededConnections(current);
    } finally {
      current.handshakeInFlight = false;
    }
  }

  private async removeSupersededConnections(
    admitted: ActiveClientServerTarget
  ): Promise<void> {
    const stableKey = clientServerStableKey(admitted.descriptor);
    const superseded = [...this.connections.values()]
      .filter(current =>
        current.connectionId !== admitted.connectionId
        && clientServerStableKey(current.descriptor) === stableKey)
      .map(current => current.connectionId);
    for (const connectionId of superseded) {
      await this.closeConnection(connectionId);
    }
  }

  private async handleConnectionTerminated(connectionId: string): Promise<void> {
    if (!this.connections.delete(connectionId)) return;
    await this.sockets.disconnectClientServerConnection(connectionId);
  }

  private async closeConnection(connectionId: string): Promise<void> {
    const current = this.connections.get(connectionId);
    if (current === undefined) return;
    this.connections.delete(connectionId);
    await this.sockets.closeClientServerConnection(connectionId);
  }

  private async drainAndRemoveServers(signal?: AbortSignal): Promise<void> {
    const owner = this.locationRuntime.currentOwnerToken;
    if (owner === undefined) return;
    for (const [channelName, descriptor] of this.localDescriptors) {
      const draining = {
        ...descriptor,
        descriptorRevision: descriptor.descriptorRevision + 1n,
        state: ZLinkFrameworkRuntimeState.Draining
      };
      const result = await this.store.updateClientServer(
        draining,
        ZLinkLocationWriteIntent.Renew,
        signal
      );
      if (result.status === ZLinkLocationWriteStatus.Stored) {
        this.sockets.setClientServerServerDescriptor(draining, channelName);
        await this.store.removeClientServer({
          channelName,
          serverRid: descriptor.serverRid
        }, owner, signal);
      }
      this.sockets.setClientServerServerDescriptor(undefined, channelName);
    }
    this.localDescriptors.clear();
  }

  private requireOwnerToken(): ZLinkLocationOwnerToken {
    const owner = this.locationRuntime.currentOwnerToken;
    if (owner === undefined) {
      throw new ZLinkConfigurationException(
        'ClientServer discovery requires the location owner lease to be started.'
      );
    }
    return owner;
  }

  private schedule(): void {
    const controller = this.controller;
    if (controller === undefined || controller.signal.aborted) return;
    this.timer = setTimeout(() => {
      this.timer = undefined;
      void this.tick(controller.signal)
        .catch((error) => this.locationRuntime.reportDiscoveryFailure(error))
        .finally(() => this.schedule());
    }, this.options.pollingIntervalMs);
    this.timer.unref();
  }
}

function clientServerStableKey(descriptor: ZLinkClientServerServerDescriptor): string {
  return `${descriptor.channelName}\0${String(descriptor.serverRid)}`;
}

function clientServerConnectionId(descriptor: ZLinkClientServerServerDescriptor): string {
  const channelName = descriptor.channelName;
  const serverRid = String(descriptor.serverRid);
  return `${channelName.length}:${channelName}:${serverRid.length}:${serverRid}:${descriptor.lifecycleGeneration}`;
}

function sameDescriptor(
  left: ZLinkClientServerServerDescriptor,
  right: ZLinkClientServerServerDescriptor
): boolean {
  return left.lifecycleGeneration === right.lifecycleGeneration
    && left.descriptorRevision === right.descriptorRevision
    && left.endpoint === right.endpoint
    && left.weight === right.weight
    && left.state === right.state
    && left.ownerId === right.ownerId
    && left.leaseGeneration === right.leaseGeneration;
}


function toDiscoveryDescriptor(
  descriptor: ZLinkClientServerServerDescriptor,
  effectiveMaxMessageBytes = 0x7fff_ffff
) {
  return {
    channelName: descriptor.channelName,
    serverRoutingId: String(descriptor.serverRid),
    lifecycleGeneration: descriptor.lifecycleGeneration,
    descriptorRevision: descriptor.descriptorRevision,
    weight: descriptor.weight,
    state: runtimeStateName(descriptor.state),
    securityIdentity: descriptor.securityIdentity,
    effectiveMaxMessageBytes,
    advertisedEndpoint: descriptor.endpoint
  };
}

function requestAdmission(
  dealer: ZLinkBackendDealerSocket,
  descriptor: ZLinkClientServerServerDescriptor,
  timeoutMs: number
): Promise<ZLinkClientServerAdmission> {
  const message = RuntimeMessage.from(encodeClientServerHello({
    channelName: descriptor.channelName,
    securityIdentity: descriptor.securityIdentity,
    normalizedEffectiveMaxMessageBytes: normalizedMessageLimit(dealer.maxMessageSize)
  }));
  return new Promise((resolve, reject) => {
    let submitted = false;
    try {
      submitted = dealer.request(
        message,
        (result, parts) => {
          try {
            if (result !== 0 || parts.length !== 1) {
              reject(new ZLinkConfigurationException(
                `ClientServer '${descriptor.channelName}' admission request failed with result ${result}.`
              ));
              return;
            }
            const first = parts[0]!;
            const record = decodeClientServerControl(first.data());
            if (record.kind === 'reject') {
              reject(new ZLinkConfigurationException(
                `ClientServer '${descriptor.channelName}' admission was rejected (${record.reason}).`
              ));
              return;
            }
            if (record.kind !== 'admit') {
              reject(new ZLinkConfigurationException(
                `ClientServer '${descriptor.channelName}' admission reply has an invalid command.`
              ));
              return;
            }
            resolve(record.admission);
          } catch (error) {
            reject(error);
          } finally {
            message.close();
            closeMessages(parts);
          }
        },
        0,
        timeoutMs
      );
    } catch (error) {
      message.close();
      reject(error);
      return;
    }
    if (!submitted) {
      message.close();
      reject(new ZLinkConfigurationException(
        `ClientServer '${descriptor.channelName}' admission request was not submitted.`
      ));
    }
  });
}

function requireMatchingAdmission(
  expected: ZLinkClientServerServerDescriptor,
  actual: ZLinkClientServerAdmission
): void {
  if (actual.channelName !== expected.channelName
    || actual.serverRid !== String(expected.serverRid)
    || actual.lifecycleGeneration !== expected.lifecycleGeneration
    || actual.descriptorRevision !== expected.descriptorRevision
    || actual.weight !== expected.weight
    || actual.state !== expected.state
    || actual.securityIdentity !== expected.securityIdentity
    || actual.advertisedEndpoint !== expected.endpoint) {
    throw new ZLinkConfigurationException(
      `ClientServer '${expected.channelName}' admission does not match its descriptor.`
    );
  }
}

function closeMessages(parts: readonly Message[]): void {
  for (const part of parts) part.close();
}

function normalizedMessageLimit(value: number): number {
  return Number.isSafeInteger(value) && value > 0
    ? Math.min(value, 0xffff_ffff)
    : 0x7fff_ffff;
}

function runtimeStateName(
  state: ZLinkFrameworkRuntimeState
): 'preparing' | 'serving' | 'retiring' | 'stopped' | 'error' {
  switch (state) {
    case ZLinkFrameworkRuntimeState.Preparing: return 'preparing';
    case ZLinkFrameworkRuntimeState.Serving: return 'serving';
    case ZLinkFrameworkRuntimeState.Relocating:
    case ZLinkFrameworkRuntimeState.Relocated:
    case ZLinkFrameworkRuntimeState.Draining: return 'retiring';
    case ZLinkFrameworkRuntimeState.Stopped: return 'stopped';
    default: return 'error';
  }
}

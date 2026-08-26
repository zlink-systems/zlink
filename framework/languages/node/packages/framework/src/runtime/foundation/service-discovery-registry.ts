export interface ClientServerDescriptor {
  readonly channelName: string;
  readonly serverRoutingId: string;
  readonly lifecycleGeneration: bigint;
  readonly descriptorRevision: bigint;
  readonly weight: number;
  readonly state: 'preparing' | 'serving' | 'retiring' | 'stopped' | 'error' | 'disconnected';
  readonly securityIdentity: string;
  readonly effectiveMaxMessageBytes: number;
  readonly advertisedEndpoint: string;
}

export interface FanoutPublisherDescriptor {
  readonly channelName: string;
  readonly publisherRoutingId: string;
  readonly lifecycleGeneration: bigint;
  readonly descriptorRevision: bigint;
  readonly advertisedEndpoint: string;
  readonly state: 'preparing' | 'serving' | 'retiring' | 'stopped' | 'error';
}

interface Current<T> {
  readonly descriptor: T;
  readonly connectionId: string;
}

export interface SelectedClientServer {
  readonly descriptor: ClientServerDescriptor;
  readonly connectionId: string;
}

/** Dedicated discovery state; fanout publishers never reuse RouteMesh peer rows. */
export class ServiceDiscoveryRegistry {
  private readonly clientServers = new Map<string, Current<ClientServerDescriptor>>();
  private readonly fanoutPublishers = new Map<string, Current<FanoutPublisherDescriptor>>();
  private readonly clientServerSelections = new Map<string, SmoothWeightedSelection<SelectedClientServer>>();

  admitClientServer(descriptor: ClientServerDescriptor, connectionId: string): boolean {
    validateClientServer(descriptor);
    const key = `${descriptor.channelName}\0${descriptor.serverRoutingId}`;
    const current = this.clientServers.get(key);
    if (current !== undefined
      && current.descriptor.state === 'disconnected'
      && sameClientServerDescriptorExceptState(current.descriptor, descriptor)) {
      this.clientServers.set(key, { descriptor: { ...descriptor }, connectionId });
      this.invalidateClientServerCandidates(descriptor.channelName);
      return true;
    }
    const admitted = this.admit(this.clientServers, key, descriptor, connectionId);
    if (admitted) this.invalidateClientServerCandidates(descriptor.channelName);
    return admitted;
  }

  markClientServerDisconnected(
    channelName: string,
    serverRoutingId: string,
    connectionId: string
  ): boolean {
    const key = `${channelName}\0${serverRoutingId}`;
    const current = this.clientServers.get(key);
    if (current === undefined || current.connectionId !== connectionId) return false;
    if (current.descriptor.state !== 'disconnected') {
      this.clientServers.set(key, {
        descriptor: { ...current.descriptor, state: 'disconnected' },
        connectionId
      });
      this.invalidateClientServerCandidates(channelName);
    }
    return true;
  }

  removeClientServer(
    channelName: string,
    serverRoutingId: string,
    connectionId: string
  ): boolean {
    const removed = removeCurrent(this.clientServers, `${channelName}\0${serverRoutingId}`, connectionId);
    if (removed) this.invalidateClientServerCandidates(channelName);
    return removed;
  }

  selectClientServer(channelName: string): ClientServerDescriptor | undefined {
    return this.selectClientServerConnectionCore(channelName)?.descriptor;
  }

  selectClientServerConnection(channelName: string): SelectedClientServer | undefined {
    return this.selectClientServerConnectionCore(channelName);
  }

  private selectClientServerConnectionCore(channelName: string): SelectedClientServer | undefined {
    let selection = this.clientServerSelections.get(channelName);
    if (selection === undefined) {
      selection = new SmoothWeightedSelection(() => this.clientServerCandidates(channelName));
      this.clientServerSelections.set(channelName, selection);
    }
    return selection.select();
  }

  clientServerDescriptors(channelName: string): readonly ClientServerDescriptor[] {
    return [...this.clientServers.values()]
      .map(value => value.descriptor)
      .filter(value => value.channelName === channelName)
      .sort((left, right) => compareOrdinal(left.serverRoutingId, right.serverRoutingId))
      .map(value => ({ ...value }));
  }

  admitFanoutPublisher(descriptor: FanoutPublisherDescriptor, connectionId: string): boolean {
    validateFanout(descriptor);
    return this.admit(
      this.fanoutPublishers,
      `${descriptor.channelName}\0${descriptor.publisherRoutingId}`,
      descriptor,
      connectionId
    );
  }

  removeFanoutPublisher(
    channelName: string,
    publisherRoutingId: string,
    connectionId: string
  ): boolean {
    return removeCurrent(
      this.fanoutPublishers,
      `${channelName}\0${publisherRoutingId}`,
      connectionId
    );
  }

  fanoutEndpoints(channelName: string): readonly FanoutPublisherDescriptor[] {
    return [...this.fanoutPublishers.values()]
      .map(value => value.descriptor)
      .filter(value => value.channelName === channelName && value.state === 'serving')
      .sort((left, right) => left.publisherRoutingId.localeCompare(right.publisherRoutingId))
      .map(value => ({ ...value }));
  }

  private admit<T extends { readonly lifecycleGeneration: bigint; readonly descriptorRevision: bigint }>(
    rows: Map<string, Current<T>>,
    key: string,
    descriptor: T,
    connectionId: string
  ): boolean {
    requireText(connectionId, 'connectionId');
    const current = rows.get(key);
    if (
      current !== undefined
      && current.descriptor.lifecycleGeneration === descriptor.lifecycleGeneration
      && (
        current.descriptor.descriptorRevision > descriptor.descriptorRevision
        || (
          current.descriptor.descriptorRevision === descriptor.descriptorRevision
          && !sameDescriptor(current.descriptor, descriptor)
        )
      )
    ) {
      return false;
    }
    rows.set(key, { descriptor: { ...descriptor }, connectionId });
    return true;
  }

  private invalidateClientServerCandidates(channelName: string): void {
    if (this.clientServerSelections.has(channelName)) {
      this.clientServerSelections.get(channelName)!.rebuild();
    }
  }

  private clientServerCandidates(channelName: string) {
    return [...this.clientServers.values()]
      .filter(value =>
        value.descriptor.channelName === channelName
        && value.descriptor.state === 'serving'
        && value.descriptor.weight > 0)
      .sort((left, right) =>
        compareOrdinal(left.descriptor.serverRoutingId, right.descriptor.serverRoutingId))
      .map(value => ({
        id: value.descriptor.serverRoutingId,
        weight: value.descriptor.weight,
        value: Object.freeze({
          descriptor: Object.freeze({ ...value.descriptor }),
          connectionId: value.connectionId
        })
      }));
  }
}

function compareOrdinal(left: string, right: string): number {
  return left < right ? -1 : left > right ? 1 : 0;
}

function sameDescriptor<T extends { readonly lifecycleGeneration: bigint; readonly descriptorRevision: bigint }>(
  left: T,
  right: T
): boolean {
  return JSON.stringify({
    ...left,
    lifecycleGeneration: left.lifecycleGeneration.toString(),
    descriptorRevision: left.descriptorRevision.toString()
  }) === JSON.stringify({
    ...right,
    lifecycleGeneration: right.lifecycleGeneration.toString(),
    descriptorRevision: right.descriptorRevision.toString()
  });
}

function sameClientServerDescriptorExceptState(
  left: ClientServerDescriptor,
  right: ClientServerDescriptor
): boolean {
  return left.channelName === right.channelName
    && left.serverRoutingId === right.serverRoutingId
    && left.lifecycleGeneration === right.lifecycleGeneration
    && left.descriptorRevision === right.descriptorRevision
    && left.weight === right.weight
    && left.securityIdentity === right.securityIdentity
    && left.effectiveMaxMessageBytes === right.effectiveMaxMessageBytes
    && left.advertisedEndpoint === right.advertisedEndpoint;
}

function removeCurrent<T>(
  rows: Map<string, Current<T>>,
  key: string,
  connectionId: string
): boolean {
  const current = rows.get(key);
  if (current === undefined || current.connectionId !== connectionId) return false;
  rows.delete(key);
  return true;
}

function validateClientServer(descriptor: ClientServerDescriptor): void {
  requireText(descriptor.channelName, 'channelName');
  requireText(descriptor.serverRoutingId, 'serverRoutingId');
  requireText(descriptor.securityIdentity, 'securityIdentity');
  requireText(descriptor.advertisedEndpoint, 'advertisedEndpoint');
  validateRevision(descriptor.lifecycleGeneration, descriptor.descriptorRevision);
  if (!Number.isInteger(descriptor.weight) || descriptor.weight < 0 || descriptor.weight > 10_000) {
    throw new RangeError('ClientServer weight must be an integer in 0..10000.');
  }
  if (!Number.isSafeInteger(descriptor.effectiveMaxMessageBytes) || descriptor.effectiveMaxMessageBytes < 1) {
    throw new RangeError('ClientServer message bound must be positive.');
  }
}

function validateFanout(descriptor: FanoutPublisherDescriptor): void {
  requireText(descriptor.channelName, 'channelName');
  requireText(descriptor.publisherRoutingId, 'publisherRoutingId');
  requireText(descriptor.advertisedEndpoint, 'advertisedEndpoint');
  validateRevision(descriptor.lifecycleGeneration, descriptor.descriptorRevision);
}

function validateRevision(lifecycle: bigint, revision: bigint): void {
  if (lifecycle <= 0n || revision <= 0n) {
    throw new RangeError('Lifecycle generation and descriptor revision must be non-zero.');
  }
}

function requireText(value: string, field: string): void {
  if (value.length === 0 || value.includes('\0')) {
    throw new TypeError(`${field} must be non-empty text without NUL.`);
  }
}
import { SmoothWeightedSelection } from './service-weighted-selection';

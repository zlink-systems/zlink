import type { ZLinkSocketConfig } from '../../contracts';
import type {
  ZLinkSocketConfig as ZLinkRuntimeSocketConfig
} from '../../contracts/Configuration';
import {
  ZLinkConfigurationException
} from '../configuration';
import type {
  ZLinkBackendRouterSocket
} from '../backend/contracts';
import { requireValidSendTimeoutMs } from '../../contracts/Configuration/SendTimeoutValidation';

interface ZLinkChannelSocketOptionsRuntime {
  clientServerServerSocket(channelName: string): ZLinkBackendRouterSocket;
  routeMeshSocket(channelName: string): ZLinkBackendRouterSocket;
  clientServerServerWeight?(channelName: string): number;
  setClientServerServerWeight?(channelName: string, weight: number): void;
  routeMeshWeight?(channelName: string): number;
  setRouteMeshWeight?(channelName: string, weight: number): void;
}

class ZLinkLiveSocketConfig implements ZLinkSocketConfig {
  constructor(
    private readonly socket: ZLinkBackendRouterSocket,
    private readonly readWeight: () => number,
    private readonly writeWeight: (weight: number) => void
  ) {}

  get weight(): number {
    return this.readWeight();
  }

  set weight(value: number) {
    validatePublicWeight(value);
    this.writeWeight(value);
  }

  get sendHighWaterMark(): number {
    return this.socket.sendHighWaterMark;
  }

  set sendHighWaterMark(value: number) {
    validateHighWaterMark(value, 'sendHighWaterMark');
    this.socket.sendHighWaterMark = value;
  }

  get receiveHighWaterMark(): number {
    return this.socket.receiveHighWaterMark;
  }

  set receiveHighWaterMark(value: number) {
    validateHighWaterMark(value, 'receiveHighWaterMark');
    this.socket.receiveHighWaterMark = value;
  }

  get sendTimeoutMs(): number {
    return this.socket.sendTimeoutMs;
  }

  set sendTimeoutMs(value: number) {
    validateSendTimeout(value);
    this.socket.sendTimeoutMs = value;
  }

  get maxMessageSize(): number {
    return this.socket.maxMessageSize;
  }

  set maxMessageSize(value: number) {
    validateMaxMessageSize(value);
    this.socket.maxMessageSize = value;
  }
}

class ZLinkServerRuntimeOptions {
  constructor(private readonly serverSocket: ZLinkSocketConfig) {}

  configureServerSocket(): ZLinkSocketConfig {
    return this.serverSocket;
  }
}

class ZLinkRouteRuntimeOptions {
  constructor(private readonly socket: ZLinkSocketConfig) {}

  configureSocket(): ZLinkSocketConfig {
    return this.socket;
  }
}

export class DefaultZLinkChannelRuntimeOptions {
  constructor(private readonly manager: () => ZLinkChannelSocketOptionsRuntime | undefined) {}

  serverChannel(channelName: string): ZLinkRuntimeSocketConfig {
    requireChannelName(channelName);
    const manager = this.requireManager();
    const socket = manager.clientServerServerSocket(channelName);
    return new ZLinkServerRuntimeOptions(
      new ZLinkLiveSocketConfig(
        socket,
        () => manager.clientServerServerWeight?.(channelName) ?? socket.peerWeight,
        (weight) => {
          if (manager.setClientServerServerWeight !== undefined) {
            manager.setClientServerServerWeight(channelName, weight);
          } else {
            socket.peerWeight = weight;
          }
        }
      )
    ).configureServerSocket();
  }

  routeChannel(channelName: string): ZLinkRuntimeSocketConfig {
    requireChannelName(channelName);
    const manager = this.requireManager();
    const socket = manager.routeMeshSocket(channelName);
    return new ZLinkRouteRuntimeOptions(
      new ZLinkLiveSocketConfig(
        socket,
        () => manager.routeMeshWeight?.(channelName) ?? socket.peerWeight,
        (weight) => {
          if (manager.setRouteMeshWeight !== undefined) {
            manager.setRouteMeshWeight(channelName, weight);
          } else {
            socket.peerWeight = weight;
          }
        }
      )
    ).configureSocket();
  }

  private requireManager(): ZLinkChannelSocketOptionsRuntime {
    const manager = this.manager();
    if (manager === undefined) {
      throw new ZLinkConfigurationException('Channel runtime is not started.');
    }
    return manager;
  }
}

function requireChannelName(channelName: string): void {
  if (channelName.trim().length === 0 || channelName.trim() !== channelName) {
    throw new ZLinkConfigurationException('Channel name must not be empty or padded.');
  }
}

function validatePublicWeight(value: number): void {
  if (!Number.isInteger(value) || value < 0 || value > 10_000) {
    throw new ZLinkConfigurationException('Weight must be an integer in 0..10000.');
  }
}

function validateHighWaterMark(value: number, label: string): void {
  if (!Number.isInteger(value) || value < 0) {
    throw new ZLinkConfigurationException(`${label} must be a non-negative integer.`);
  }
}

function validateSendTimeout(value: number): void {
  requireValidSendTimeoutMs('sendTimeoutMs', value);
}

function validateMaxMessageSize(value: number): void {
  if (!Number.isInteger(value) || value < 0) {
    throw new ZLinkConfigurationException('maxMessageSize must be a non-negative integer.');
  }
}

import type { ZLinkEndpointConnections } from './Connections';
import { normalizeEndpoint } from './EndpointNotation';

interface ConnectableEndpointSocket {
  connect(endpoint: string): void;
  disconnect(endpoint: string): void;
}

const controllers = new WeakMap<object, RuntimeEndpointConnections>();

class RuntimeEndpointConnections implements ZLinkEndpointConnections {
  private socket?: ConnectableEndpointSocket;
  private readonly endpoints: string[];

  constructor(endpoints: readonly string[]) {
    if (Array.isArray(endpoints) && Object.isExtensible(endpoints)) {
      // Builder registration and the public handle must observe one mutable
      // endpoint set. A frozen, sealed or external readonly array uses a
      // private snapshot because it cannot safely be updated in place.
      this.endpoints = endpoints as string[];
      // Normalize in place -- callers (and the builder that shares this
      // array) must keep observing the same array identity.
      for (let index = 0; index < this.endpoints.length; index += 1) {
        this.endpoints[index] = normalizeEndpoint(this.endpoints[index]);
      }
      for (let index = this.endpoints.length - 1; index >= 0; index -= 1) {
        if (this.endpoints.indexOf(this.endpoints[index]) !== index) {
          this.endpoints.splice(index, 1);
        }
      }
    } else {
      this.endpoints = [];
      for (const endpoint of endpoints) {
        const normalizedEndpoint = normalizeEndpoint(endpoint);
        if (!this.endpoints.includes(normalizedEndpoint)) {
          this.endpoints.push(normalizedEndpoint);
        }
      }
    }
  }

  connect(endpoint: string): void {
    const normalizedEndpoint = normalizeEndpoint(endpoint);
    if (!this.endpoints.includes(normalizedEndpoint)) {
      this.endpoints.push(normalizedEndpoint);
      this.socket?.connect(normalizedEndpoint);
    }
  }

  disconnect(endpoint: string): void {
    // Normalize so a caller using a different (but equivalent) notation than
    // the one recorded at connect() time still matches -- otherwise this is
    // a silent no-op from the caller's perspective.
    const normalizedEndpoint = normalizeEndpoint(endpoint);
    const index = this.endpoints.indexOf(normalizedEndpoint);
    if (index >= 0) {
      this.endpoints.splice(index, 1);
      this.socket?.disconnect(normalizedEndpoint);
    }
  }

  listConnections(): readonly string[] {
    return Object.freeze([...this.endpoints]);
  }

  attach(socket: ConnectableEndpointSocket): void {
    this.socket = socket;
  }

  detach(): void {
    this.socket = undefined;
  }
}

export function endpointConnections(owner: object, endpoints: readonly string[]): ZLinkEndpointConnections {
  let controller = controllers.get(owner);
  if (controller === undefined) {
    controller = new RuntimeEndpointConnections(endpoints);
    controllers.set(owner, controller);
  }
  return controller;
}

export function attachEndpointConnections(owner: object, socket: ConnectableEndpointSocket): void {
  const controller = controllers.get(owner);
  controller?.attach(socket);
}

export function detachEndpointConnections(owner: object): void {
  controllers.get(owner)?.detach();
}

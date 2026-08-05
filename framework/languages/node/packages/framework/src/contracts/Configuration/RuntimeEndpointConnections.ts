import type { ZLinkEndpointConnections } from './Connections';

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
      for (let index = this.endpoints.length - 1; index >= 0; index -= 1) {
        if (this.endpoints.indexOf(this.endpoints[index]) !== index) {
          this.endpoints.splice(index, 1);
        }
      }
    } else {
      this.endpoints = [];
      for (const endpoint of endpoints) {
        if (!this.endpoints.includes(endpoint)) {
          this.endpoints.push(endpoint);
        }
      }
    }
  }

  connect(endpoint: string): void {
    if (!this.endpoints.includes(endpoint)) {
      this.endpoints.push(endpoint);
      this.socket?.connect(endpoint);
    }
  }

  disconnect(endpoint: string): void {
    const index = this.endpoints.indexOf(endpoint);
    if (index >= 0) {
      this.endpoints.splice(index, 1);
      this.socket?.disconnect(endpoint);
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

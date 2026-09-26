import type { ZLinkListenerKind } from '../../contracts/RouteMesh/RuntimeTopology';

/**
 * Bound listener records of one runtime generation (spec 04 §3.1). A listener writes its
 * record when it finishes binding. The listener status query reads only this object.
 */
export class ZLinkListenerRecords {
  private readonly endpoints = new Map<string, string>();

  record(kind: ZLinkListenerKind, name: string, endpoint: string): void {
    this.endpoints.set(`${kind}:${name}`, endpoint);
  }

  clear(): void {
    this.endpoints.clear();
  }

  endpoint(kind: ZLinkListenerKind, name: string): string | undefined {
    return this.endpoints.get(`${kind}:${name}`);
  }
}

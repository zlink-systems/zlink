export type ZLinkRouteMemberStatus = 'unknown' | 'missing' | 'connected' | 'disconnected';

export class ZLinkRouteMemberSnapshot {
  private readonly members = new Map<string, Set<string>>();
  private readonly connected = new Map<string, Set<string>>();
  private readonly memberByEndpoint = new Map<string, Map<string, string>>();

  clear(): void {
    this.members.clear();
    this.connected.clear();
    this.memberByEndpoint.clear();
  }

  observeReady(channel: string, routingId: string, endpoint: string): void {
    getOrCreateSet(this.members, channel).add(routingId);
    getOrCreateSet(this.connected, channel).add(routingId);
    if (endpoint.length > 0) getOrCreateMap(this.memberByEndpoint, channel).set(endpoint, routingId);
  }

  observeTermination(channel: string, routingId: string | undefined, endpoint: string): string | undefined {
    const connected = this.connected.get(channel);
    const disconnected = routingId
      ?? this.memberByEndpoint.get(channel)?.get(endpoint)
      ?? (connected?.size === 1 ? connected.values().next().value : undefined);
    if (disconnected === undefined) return undefined;
    getOrCreateSet(this.members, channel).add(disconnected);
    connected?.delete(disconnected);
    return disconnected;
  }

  status(channel: string, routingId: string): ZLinkRouteMemberStatus {
    const members = this.members.get(channel);
    if (members === undefined || members.size === 0) return 'unknown';
    if (!members.has(routingId)) return 'missing';
    return this.connected.get(channel)?.has(routingId) === true ? 'connected' : 'disconnected';
  }
}

function getOrCreateSet(map: Map<string, Set<string>>, key: string): Set<string> {
  let values = map.get(key);
  if (values === undefined) {
    values = new Set<string>();
    map.set(key, values);
  }
  return values;
}

function getOrCreateMap(map: Map<string, Map<string, string>>, key: string): Map<string, string> {
  let values = map.get(key);
  if (values === undefined) {
    values = new Map<string, string>();
    map.set(key, values);
  }
  return values;
}

export interface ClientEndpoints {
  gateway: string;
  ops: string;
}

export async function loadClientEndpoints(): Promise<ClientEndpoints> {
  const response = await fetch('/config.json', { cache: 'no-store' });
  if (!response.ok) {
    throw new Error(`Unable to load /config.json: HTTP ${response.status}`);
  }

  const value = await response.json() as Partial<ClientEndpoints>;
  return {
    gateway: requireWebSocketEndpoint(value.gateway, 'gateway'),
    ops: requireWebSocketEndpoint(value.ops, 'ops'),
  };
}

function requireWebSocketEndpoint(value: unknown, name: string): string {
  if (typeof value !== 'string') {
    throw new Error(`/config.json must contain a string '${name}' endpoint.`);
  }

  const endpoint = new URL(value);
  if (endpoint.protocol !== 'ws:' && endpoint.protocol !== 'wss:') {
    throw new Error(`/config.json '${name}' must use ws or wss.`);
  }
  return endpoint.toString().replace(/\/$/, '');
}

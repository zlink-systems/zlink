import { RoleConfig } from '../shared/contracts';

// The common runner reserves every listener as tcp://127.0.0.1:<port>. The Node.js STREAM transport is a WebSocket
// (interfaces/06-stream-worker.ko.md), so the STREAM node binds the same host and port under the ws scheme.
export function streamEndpoint(config: RoleConfig): string {
  return config.transportEndpoints.stream.replace(/^tcp:\/\//, 'ws://');
}

export function port(endpoint: string): number {
  return Number(new URL(endpoint.replace(/^tcp:/, 'http:')).port);
}

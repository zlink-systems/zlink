import type { HttpRoute } from '../Support/http-server';

export function createOperationalEndpoints(role: string, rid: string, stop: () => void): readonly HttpRoute[] {
  return [
    { method: 'GET', path: '/health', handle: () => ({ status: 'ready', role, rid }) },
    { method: 'POST', path: '/shutdown', handle: () => { stop(); return { status: 'stopping' }; } }
  ];
}

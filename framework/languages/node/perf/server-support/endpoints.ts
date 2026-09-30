export function port(endpoint: string): number {
  return Number(new URL(endpoint.replace(/^tcp:/, 'http:')).port);
}

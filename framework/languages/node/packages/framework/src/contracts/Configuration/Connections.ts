export interface ZLinkEndpointConnections {
  connect(endpoint: string): void;
  disconnect(endpoint: string): void;
  listConnections(): readonly string[];
}

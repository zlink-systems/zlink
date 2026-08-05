import {
  ZlinkStreamErrorCode,
  ZlinkStreamTransport
} from '../../Contracts';
import { connectorError } from '../ZlinkStreamSupport';

export function inferTransport(endpoint: string): ZlinkStreamTransport {
  const url = parseEndpoint(endpoint);
  switch (url.protocol) {
    case 'ws:':
      return ZlinkStreamTransport.WebSocket;
    case 'wss:':
      return ZlinkStreamTransport.WebSocketSecure;
    default:
      throw connectorError(
        ZlinkStreamErrorCode.ConfigurationError,
        'The TypeScript Stream Connector supports only ws:// and wss:// endpoints.'
      );
  }
}

export function parseEndpoint(endpoint: string): URL {
  try {
    return new URL(endpoint);
  } catch (cause) {
    throw connectorError(ZlinkStreamErrorCode.ConfigurationError, 'Endpoint is invalid.', cause);
  }
}

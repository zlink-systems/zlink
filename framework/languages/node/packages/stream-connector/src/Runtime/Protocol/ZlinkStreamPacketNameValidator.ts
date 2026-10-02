import {
  validateStreamWirePacketName,
  ZLINK_STREAM_RESERVED_PACKET_NAME_PREFIX
} from '@zlink-systems/stream-wire';
import { ZlinkStreamErrorCode } from '../../Contracts';
import { connectorError } from '../ZlinkStreamSupport';

export function validateName(name: string, allowReserved = false): void {
  try {
    validateStreamWirePacketName(name);
  } catch (cause) {
    throw connectorError(ZlinkStreamErrorCode.ValidationFailed, 'Message name is invalid.', cause);
  }
  if (!allowReserved && name.startsWith(ZLINK_STREAM_RESERVED_PACKET_NAME_PREFIX)) {
    throw connectorError(
      ZlinkStreamErrorCode.ValidationFailed,
      'Message name uses a reserved zlink prefix.'
    );
  }
}

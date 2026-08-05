import { ZlinkStreamErrorCode } from '../../Contracts';
import { connectorError, utf8Encode } from '../ZlinkStreamSupport';

export function validateName(name: string, allowReserved = false): void {
  if (name.length === 0) {
    throw connectorError(ZlinkStreamErrorCode.ValidationFailed, 'Message name must not be empty.');
  }
  if (!allowReserved && name.startsWith('$zlink.')) {
    throw connectorError(ZlinkStreamErrorCode.ValidationFailed, 'Message name uses a reserved zlink prefix.');
  }
  if (utf8Encode(name).length > 255) {
    throw connectorError(ZlinkStreamErrorCode.ValidationFailed, 'Message name must not exceed 255 UTF-8 bytes.');
  }
}

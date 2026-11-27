import { ZLinkConfigurationException } from './ConfigurationException';

export function decodeAdmissionQueueKey(value: unknown, option: string): Buffer {
  if (typeof value === 'string' && value.length >= 43 && value.length <= 86) {
    const bytes = Buffer.from(value, 'base64url');
    if (bytes.length >= 32 && bytes.length <= 64 && bytes.toString('base64url') === value) {
      return bytes;
    }
  }
  throw new ZLinkConfigurationException(
    `${option} must be an unpadded base64url string encoding 32 to 64 bytes.`
  );
}

import { ZLinkConfigurationException } from './ConfigurationException';

const minKeySize = 32;
const maxKeySize = 64;
const minKeyEncodedSize = Math.ceil((minKeySize * 4) / 3);
const maxKeyEncodedSize = Math.ceil((maxKeySize * 4) / 3);

export function decodeAdmissionQueueKey(value: unknown, option: string): Buffer {
  if (
    typeof value === 'string' &&
    value.length >= minKeyEncodedSize &&
    value.length <= maxKeyEncodedSize
  ) {
    const bytes = Buffer.from(value, 'base64url');
    if (
      bytes.length >= minKeySize &&
      bytes.length <= maxKeySize &&
      bytes.toString('base64url') === value
    ) {
      return bytes;
    }
  }
  throw new ZLinkConfigurationException(
    `${option} must be an unpadded base64url string encoding ${minKeySize} to ${maxKeySize} bytes.`
  );
}

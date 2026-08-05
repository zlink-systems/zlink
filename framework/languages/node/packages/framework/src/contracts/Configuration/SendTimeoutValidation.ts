import { ZLinkConfigurationException } from './ConfigurationException';

export const ZLINK_MAX_SEND_TIMEOUT_MS = 2_147_483_647;

export function requireValidSendTimeoutMs(label: string, value: number | undefined): void {
  if (value === undefined) return;
  if (!Number.isFinite(value)
    || !Number.isInteger(value)
    || value < 1
    || value > ZLINK_MAX_SEND_TIMEOUT_MS) {
    throw new ZLinkConfigurationException(
      `${label} must be an integer between 1 and ${ZLINK_MAX_SEND_TIMEOUT_MS} milliseconds.`
    );
  }
}

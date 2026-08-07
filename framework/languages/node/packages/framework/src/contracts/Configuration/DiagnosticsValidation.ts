import type { ZLinkMessageFlowLogMode } from '../Dispatch';
import { ZLinkConfigurationException } from './ConfigurationException';

const MESSAGE_FLOW_MODES: ReadonlySet<string> = new Set([
  'off',
  'errors',
  'normal',
  'detailed'
]);

export function requireMessageFlowLogMode(value: unknown): ZLinkMessageFlowLogMode {
  if (typeof value !== 'string' || !MESSAGE_FLOW_MODES.has(value)) {
    throw new ZLinkConfigurationException(
      "messageFlow must be one of 'off', 'errors', 'normal', or 'detailed'."
    );
  }
  return value as ZLinkMessageFlowLogMode;
}

export function requireTraceSampleRate(value: number): number {
  if (!Number.isFinite(value) || value < 0 || value > 1) {
    throw new ZLinkConfigurationException('traceSampleRate must be a finite number in 0..1.');
  }
  return value;
}

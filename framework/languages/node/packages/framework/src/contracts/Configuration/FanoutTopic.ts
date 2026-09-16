import { ZLinkConfigurationException } from './ConfigurationException';

export const FANOUT_LIVENESS_TOPIC = '\x01ZLF1';

export function requirePublicFanoutTopic(topic: string): void {
  if (typeof topic !== 'string') {
    throw new ZLinkConfigurationException('Fanout topic must be a string.');
  }
  if (topic.startsWith(FANOUT_LIVENESS_TOPIC)) {
    throw new ZLinkConfigurationException('Fanout topic is reserved for framework liveness.');
  }
}

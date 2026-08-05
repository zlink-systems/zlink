import { objectValues, requiredString } from '../../../configuration';
export interface TriggerOptions { readonly httpUrl: string; readonly serviceChannelEndpoint: string; readonly serviceBChannelEndpoint: string; readonly replacementServiceChannelEndpoint: string; readonly throwChannelEndpoint: string; readonly logDir: string; }
export function validateTriggerOptions(value: unknown): TriggerOptions { const values = objectValues(value); return {
  httpUrl: requiredString(values, 'httpUrl'), serviceChannelEndpoint: requiredString(values, 'serviceChannelEndpoint'),
  serviceBChannelEndpoint: requiredString(values, 'serviceBChannelEndpoint'),
  replacementServiceChannelEndpoint: requiredString(values, 'replacementServiceChannelEndpoint'),
  throwChannelEndpoint: requiredString(values, 'throwChannelEndpoint'), logDir: requiredString(values, 'logDir')
}; }

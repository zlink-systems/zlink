export interface DelayOptions {
  readonly rid: string;
  readonly httpUrl: string;
  readonly delayEndpoint: string;
  readonly evidenceFile?: string;
}

export const DELAY_OPTIONS = Symbol.for('AUTOMATIC_TURN_DELAY_OPTIONS');

export function validateDelayOptions(value: unknown): DelayOptions {
  const values = objectValue(value);
  return {
    rid: requiredString(values, 'rid'),
    httpUrl: requiredString(values, 'httpUrl'),
    delayEndpoint: requiredString(values, 'delayEndpoint'),
    evidenceFile: optionalString(values, 'evidenceFile')
  };
}
import { objectValue, optionalString, requiredString } from '../../../configuration';

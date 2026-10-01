import { ZLinkTimerOverrunPolicy, type ZLinkTimerOptions } from '../Timers';
import { ZLinkConfigurationException } from './ConfigurationException';

const MAX_TIMER_CATCH_UP_TICKS = 2_147_483_647;

export function validateTimerRegistration(
  name: string,
  periodMs: number,
  options: ZLinkTimerOptions | undefined
): void {
  if (name.trim().length === 0) {
    throw new ZLinkConfigurationException('SPOT timer name must not be empty.');
  }
  if (!Number.isFinite(periodMs) || periodMs <= 0) {
    throw new ZLinkConfigurationException('SPOT timer period must be greater than zero.');
  }
  if (
    options?.overrunPolicy !== undefined &&
    !Object.values(ZLinkTimerOverrunPolicy).includes(options.overrunPolicy)
  ) {
    throw new ZLinkConfigurationException('SPOT timer overrun policy is not supported.');
  }
  if (
    options?.overrunPolicy === ZLinkTimerOverrunPolicy.CatchUpBounded &&
    (options.maxCatchUpTicks === undefined ||
      !Number.isInteger(options.maxCatchUpTicks) ||
      options.maxCatchUpTicks < 1 ||
      options.maxCatchUpTicks > MAX_TIMER_CATCH_UP_TICKS)
  ) {
    throw new ZLinkConfigurationException(
      `SPOT timer MaxCatchUpTicks must be an integer from 1 through ${MAX_TIMER_CATCH_UP_TICKS.toLocaleString('en-US')}.`
    );
  }
}

import type {
  ZLinkDispatchErrorReason,
  ZLinkDispatchFailure
} from '../../contracts/Dispatch/ZLinkDispatchOptions';
import type { ZLinkDispatchErrorReporter } from '../channels/dispatch-error-reporter';
import { dispatchReasonFromError } from '../diagnostics/dispatch-error-details';

export function reportSpotPacketFailure(
  reporter: ZLinkDispatchErrorReporter | undefined,
  packet: Omit<ZLinkDispatchFailure, 'reason'>,
  error: unknown
): ZLinkDispatchErrorReason {
  const reason = dispatchReasonFromError(error);
  if (reporter?.captureEnabled() === true) reporter.report({ ...packet, reason, error });
  return reason;
}

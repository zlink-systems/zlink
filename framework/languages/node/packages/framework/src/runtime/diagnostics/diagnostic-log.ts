import { dispatchErrorDetails } from './dispatch-error-details';
import {
  messageFlowDelivery,
  openTelemetryLease,
  type ZLinkDiagnosticRecord,
  type ZLinkTelemetryLease
} from './message-flow-delivery';

export type ZLinkDiagnosticLease = () => ZLinkTelemetryLease;

let processLease: ZLinkTelemetryLease | undefined;

export function logDiagnostic(
  lease: ZLinkTelemetryLease | undefined,
  level: ZLinkDiagnosticRecord['level'],
  category: string,
  marker: string,
  entry: Pick<ZLinkDiagnosticRecord, 'message' | 'detail'> & { readonly error?: unknown } = {}
): void {
  const owner = messageFlowDelivery();
  let detail: ZLinkDiagnosticRecord['detail'];
  if (entry.detail !== undefined) {
    try {
      detail = structuredClone(entry.detail);
    } catch {
      detail = { detailUnavailable: true };
    }
  }
  let error: ZLinkDiagnosticRecord['error'];
  if (entry.error !== undefined) {
    try {
      error = dispatchErrorDetails(entry.error);
    } catch {
      // A thrown value can expose accessors that throw while it is being inspected.
      error = { errorType: typeof entry.error };
    }
  }
  owner.submit(lease ?? (processLease ??= openTelemetryLease()), {
    level,
    category,
    marker,
    message: entry.message,
    detail,
    error,
    timestamp: new Date()
  });
}

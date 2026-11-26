import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException
} from '../framework-errors-internal';

export function createObjectOperationDeadline(
  timeoutMs: number,
  operation: 'Actor creation' | 'Actor destroy' | 'User Spot creation',
  parent?: AbortSignal
): { readonly signal: AbortSignal; close(): void } {
  const deadline = new AbortController();
  const timer = setTimeout(
    () =>
      deadline.abort(
        createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.DeadlineExceeded,
          `${operation} exceeded its end-to-end deadline.`
        )
      ),
    timeoutMs
  );
  return {
    signal: parent === undefined ? deadline.signal : AbortSignal.any([parent, deadline.signal]),
    close: () => clearTimeout(timer)
  };
}

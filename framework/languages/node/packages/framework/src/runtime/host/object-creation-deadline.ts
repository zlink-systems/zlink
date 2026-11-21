import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException
} from '../framework-errors-internal';

export function createObjectCreationDeadline(
  timeoutMs: number,
  objectKind: 'Actor' | 'User Spot',
  parent?: AbortSignal
): { readonly signal: AbortSignal; close(): void } {
  const deadline = new AbortController();
  const timer = setTimeout(
    () =>
      deadline.abort(
        createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.DeadlineExceeded,
          `${objectKind} creation exceeded its end-to-end deadline.`
        )
      ),
    timeoutMs
  );
  return {
    signal: parent === undefined ? deadline.signal : AbortSignal.any([parent, deadline.signal]),
    close: () => clearTimeout(timer)
  };
}

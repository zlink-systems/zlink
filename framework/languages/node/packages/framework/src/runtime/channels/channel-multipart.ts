import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException,
  requestResultToPublicErrorKind
} from '../framework-errors-internal';
import type { Message } from '../../contracts/Common/Message';
import { type ZLinkBackendMessageLike as MessageLike, isZLinkBackendResultError } from '../backend/runtime-values';
import { ZLinkConfigurationException } from '../configuration';
import { ZLinkFrameworkException } from '../../contracts';

export interface ZLinkMultipartOperation<TNext> {
  message(message: MessageLike): TNext;
}

export interface ZLinkMultipartSubmitOperation extends ZLinkMultipartOperation<ZLinkMultipartSubmitOperation> {
  submit(): unknown;
}

export type ZLinkMultipartReplyOperation = ZLinkMultipartSubmitOperation;

export function appendParts<TNext extends ZLinkMultipartOperation<TNext>>(
  operation: ZLinkMultipartOperation<TNext>,
  parts: readonly MessageLike[]
): TNext {
  if (parts.length === 0) {
    throw new ZLinkConfigurationException('Channel multipart envelope must contain at least one part.');
  }
  let current: TNext = operation.message(parts[0]);
  for (let index = 1; index < parts.length; index++) {
    current = current.message(parts[index]);
  }
  return current;
}

export async function submitRequestOperation(operation: {
  submit(): Promise<readonly Message[]>;
}, label: string): Promise<readonly Message[]> {
  try {
    return await operation.submit();
  } catch (error) {
    //  Spec 32-framework-error-model:81-92 — classify the backend request
    //  terminal rather than collapsing every failure to Unavailable.
    if (isZLinkBackendResultError(error) && error.operation === 'request') {
      throw new ZLinkFrameworkException(
        requestResultToPublicErrorKind(error.result),
        `${label} failed with result ${error.result}.`,
        error
      );
    }
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.RouteNotConnected,
      `${label} failed before a reply was received.`,
      true,
      error
    );
  }
}

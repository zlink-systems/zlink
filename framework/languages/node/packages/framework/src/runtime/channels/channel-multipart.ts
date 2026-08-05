import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException  } from '../framework-errors-internal';
import type { Message } from '../../contracts/Common/Message';
import type { ZLinkBackendMessageLike as MessageLike } from '../backend/runtime-values';
import { ZLinkConfigurationException } from '../configuration';

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

export function submitRequestOperation(operation: {
  submit(callback: (result: number, parts: readonly Message[]) => void): boolean;
}, label: string): Promise<readonly Message[]> {
  return new Promise((resolve, reject) => {
    try {
      const accepted = operation.submit((result, parts) => {
        if (result !== 0) {
          reject(createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.RouteNotConnected,
            `Channel request failed with result ${result}.`,
            true
          ));
          return;
        }
        resolve(parts);
      });
      if (!accepted) {
        reject(createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.RouteNotConnected,
          'Channel request submit was not accepted.',
          true
        ));
      }
    } catch (error) {
      reject(createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.RouteNotConnected,
        `${label} failed before a reply was received.`,
        true,
        error
      ));
    }
  });
}

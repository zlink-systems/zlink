/* SPDX-License-Identifier: Apache-2.0 */

import { ZLinkFrameworkException, ZLinkFrameworkErrorKind } from '@zlink-systems/framework';

export function responseBodySizeExceeded(message = 'HTTP response exceeded the maximum body size'):
  ZLinkFrameworkException {
  return new ZLinkFrameworkException(ZLinkFrameworkErrorKind.Rejected, message);
}

export function redirectLimitExceeded(message = 'HTTP request exceeded the redirect limit'):
  ZLinkFrameworkException {
  return new ZLinkFrameworkException(ZLinkFrameworkErrorKind.ProtocolError, message);
}

export function redirectFormatError(message: string): ZLinkFrameworkException {
  return new ZLinkFrameworkException(ZLinkFrameworkErrorKind.ProtocolError, message);
}

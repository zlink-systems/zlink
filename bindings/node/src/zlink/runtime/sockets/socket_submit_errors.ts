// SPDX-License-Identifier: MPL-2.0

import {
  SendFlags,
} from '../../contracts/sockets/socket_constants';
import {
  SubmitError,
  SubmitResult,
} from '../../contracts/errors/errors';
import { withRuntimeErrorMessage } from '../errors/error_state';

export function submitErrorFromResult(result: SubmitResult, message: string): SubmitError {
  return withRuntimeErrorMessage(new SubmitError(result, 0), message);
}

export function normalizeReplyFlags(flags: SendFlags = SendFlags.None): SendFlags {
  const normalized = flags | 0;
  if (normalized !== SendFlags.None) {
    throw submitErrorFromResult(
      SubmitResult.NotSupported,
      'reply flags are not supported by the current core library'
    );
  }
  return normalized as SendFlags;
}

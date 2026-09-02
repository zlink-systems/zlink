// SPDX-License-Identifier: MPL-2.0

import {
  SubmitError,
  SubmitResult,
} from '../../contracts/errors/errors';
import { withRuntimeErrorMessage } from '../errors/error_state';

export function submitErrorFromResult(result: SubmitResult, message: string): SubmitError {
  return withRuntimeErrorMessage(new SubmitError(result, 0), message);
}

export function submitErrorFromNativeResult(
  result: number,
  nativeErrno: number,
  message: string
): SubmitError {
  return withRuntimeErrorMessage(
    new SubmitError(result as SubmitResult, nativeErrno),
    message
  );
}

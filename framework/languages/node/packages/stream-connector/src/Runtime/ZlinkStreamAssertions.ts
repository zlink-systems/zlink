import {
  ZlinkStreamErrorCode,
  type ZlinkStreamError
} from '../Contracts';
import { connectorError, unwrapStreamError } from './ZlinkStreamSupport';

export interface ZlinkStreamAssertions {
  ensure(condition: boolean, message: string): asserts condition;
  expectFailure(
    action: (signal?: AbortSignal) => Promise<void>,
    errorKind?: string
  ): Promise<ZlinkStreamError>;
  expectTimeout(action: (signal?: AbortSignal) => Promise<void>): Promise<void>;
}

export const zlinkStreamAssert: ZlinkStreamAssertions = {
  ensure(condition: boolean, message: string): asserts condition {
    if (!condition) {
      throw connectorError(ZlinkStreamErrorCode.ValidationFailed, message);
    }
  },

  async expectFailure(
    action: (signal?: AbortSignal) => Promise<void>,
    errorKind?: string
  ): Promise<ZlinkStreamError> {
    let failure: unknown;
    try {
      await action();
    } catch (error) {
      failure = error;
    }
    if (failure === undefined) {
      throw connectorError(ZlinkStreamErrorCode.ValidationFailed, 'Expected action to fail.');
    }
    const streamError = unwrapStreamError(failure);
    if (errorKind !== undefined && streamError.code !== errorKind) {
      throw connectorError(
        ZlinkStreamErrorCode.ValidationFailed,
        `Expected failure kind '${errorKind}', got '${streamError.code}'.`,
        failure
      );
    }
    return streamError;
  },

  async expectTimeout(action: (signal?: AbortSignal) => Promise<void>): Promise<void> {
    let failure: unknown;
    try {
      await action();
    } catch (error) {
      failure = error;
    }
    if (failure === undefined) {
      throw connectorError(ZlinkStreamErrorCode.ValidationFailed, 'Expected action to time out.');
    }
    const code = unwrapStreamError(failure).code;
    if (code !== ZlinkStreamErrorCode.RequestTimeout && code !== ZlinkStreamErrorCode.ConnectTimeout) {
      throw failure;
    }
  }
};

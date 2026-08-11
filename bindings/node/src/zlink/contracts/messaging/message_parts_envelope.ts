// SPDX-License-Identifier: MPL-2.0

import { RecvError, RecvResult } from '../errors/errors';
import { Message } from './message';

function freezeMessageParts(parts: readonly Message[]): Message[] {
  return Object.freeze(parts.slice()) as Message[];
}

const EMPTY_MESSAGE_PARTS = freezeMessageParts([]);

function invalidMultipartError(partsLength: number): RecvError {
  const error = new RecvError(RecvResult.NotSupported, 0);
  error.message = `expected exactly 1 part but received ${partsLength}`;
  return error;
}

function missingPartError(): RecvError {
  const error = new RecvError(RecvResult.NotSupported, 0);
  error.message = 'message has no parts';
  return error;
}

export abstract class MessagePartsEnvelope {
  /** The message parts, owned by this envelope. */
  parts: Message[];

  protected constructor() {
    this.parts = EMPTY_MESSAGE_PARTS;
  }

  /** Return true when the envelope holds exactly one part. */
  isSinglePart(): boolean {
    return this.parts.length === 1;
  }

  /** Return the first part without transferring ownership; throws when the envelope has no parts. */
  firstPart(): Message {
    if (this.parts.length === 0) {
      throw missingPartError();
    }
    return this.parts[0];
  }

  /** Return the only part; throws unless the envelope holds exactly one part. */
  singlePartOrThrow(): Message {
    if (!this.isSinglePart()) {
      throw invalidMultipartError(this.parts.length);
    }
    return this.parts[0];
  }

  /** Close every part, releasing their storage. */
  close(): void {
    for (const part of this.parts) {
      part.close();
    }
    // Drop aliases before a returned Message facade can be acquired again.
    this.parts = EMPTY_MESSAGE_PARTS;
  }
}

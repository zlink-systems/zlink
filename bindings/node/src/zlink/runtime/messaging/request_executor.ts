// SPDX-License-Identifier: MPL-2.0

import { Message } from '../../contracts';
import { messageFromSnapshot } from './message_snapshot';

export function messagesFromNativeBuffers(
  buffers: readonly Buffer[] | null | undefined
): Message[] {
  return (buffers ?? []).map((buffer) =>
    messageFromSnapshot({ data: buffer ?? Buffer.alloc(0) })
  );
}

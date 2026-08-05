// SPDX-License-Identifier: MPL-2.0

import type { Message } from '../../contracts/messaging/message';

export function freezeMessageParts(parts: readonly Message[]): Message[] {
  return Object.freeze(parts.slice()) as Message[];
}

export function freezeOwnedMessageParts(parts: Message[]): Message[] {
  return Object.freeze(parts) as Message[];
}

export function closeMessageParts(parts: readonly Message[] | undefined): void {
  if (!parts) {
    return;
  }
  for (const part of parts) {
    part.close();
  }
}

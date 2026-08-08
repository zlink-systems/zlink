import type { Message } from '../../contracts/Common/Message';
import {
  decodeApplicationPayload,
  encodeApplicationPayload
} from '../foundation/service-wire-m6a-codec';
import { frameworkPayloadContentType } from './payload-codec';

const ACTOR_JOIN_MAGIC = Buffer.from([0x5a, 0x4c, 0x41, 0x4a]);
const ACTOR_JOIN_PACKET_NAME = 'ZLinkFrameworkActorJoinRequest';

export interface ZLinkActorJoinPayload {
  readonly payload: Buffer;
  readonly contentType: string;
}

export function encodeFrameworkActorJoinPayload(request: Message): Buffer {
  const envelope = encodeApplicationPayload({
    packetName: ACTOR_JOIN_PACKET_NAME,
    contentType: frameworkPayloadContentType(request),
    payload: request.data()
  });
  return Buffer.concat([ACTOR_JOIN_MAGIC, envelope]);
}

export function decodeFrameworkActorJoinPayload(
  payload: Uint8Array,
  fallbackContentType = 'application/json'
): ZLinkActorJoinPayload {
  if (!Buffer.from(payload.subarray(0, ACTOR_JOIN_MAGIC.length)).equals(ACTOR_JOIN_MAGIC)) {
    return { payload: Buffer.from(payload), contentType: fallbackContentType };
  }
  const application = decodeApplicationPayload(payload.subarray(ACTOR_JOIN_MAGIC.length));
  if (application.packetName !== ACTOR_JOIN_PACKET_NAME) {
    throw new RangeError('Actor Join payload has an unexpected packet name.');
  }
  return {
    payload: Buffer.from(application.payload),
    contentType: application.contentType
  };
}

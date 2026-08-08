import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException } from '../framework-errors-internal';
import { ZLinkBufferMessage as RuntimeMessage } from '../backend/runtime-message';
import type { ZLinkMessageSerializer } from '../../contracts';
import {
  decodeApplicationPayload,
  encodeApplicationPayload
} from '../foundation/service-wire-m6a-codec';
import {
  decodeFrameworkPayloadMessage,
  encodeFrameworkPayload,
  type ZLinkSerializerRegistryLike
} from './payload-codec';

const CREATION_PACKET_NAME = 'ZLinkFrameworkCreationRequest';

export function encodeFrameworkCreationPayload(
  payload: unknown,
  registry?: ZLinkSerializerRegistryLike | ReadonlyMap<string, ZLinkMessageSerializer>
): Buffer {
  const encoded = encodeFrameworkPayload(payload ?? null, registry);
  try {
    return encodeApplicationPayload({
      packetName: CREATION_PACKET_NAME,
      contentType: encoded.contentType,
      payload: encoded.message.data()
    });
  } finally {
    encoded.message.close();
  }
}

export function decodeFrameworkCreationPayload<T>(
  payload: Uint8Array,
  registry?: ZLinkSerializerRegistryLike | ReadonlyMap<string, ZLinkMessageSerializer>
): T {
  try {
    const application = decodeApplicationPayload(payload);
    if (application.packetName !== CREATION_PACKET_NAME) {
      throw new RangeError('Creation payload has an unexpected packet name.');
    }
    const message = RuntimeMessage.from(application.payload);
    try {
      return decodeFrameworkPayloadMessage<T>(
        message,
        registry,
        undefined,
        application.contentType
      );
    } finally {
      message.close();
    }
  } catch (error) {
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.PayloadDecodeFailed,
      'Framework creation payload is invalid.',
      error
    );
  }
}

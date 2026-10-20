import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException
} from '../framework-errors-internal';
import { ZLinkBufferMessage as RuntimeMessage } from '../backend/runtime-message';
import type { ZLinkMessageSerializer } from '../../contracts';
import {
  decodeApplicationPayloadView,
  encodeApplicationPayload
} from '../foundation/service-wire-m6a-codec';
import {
  decodeFrameworkPayloadMessage,
  encodeFrameworkPayloadMessage,
  frameworkPayloadContentType,
  type ZLinkSerializerRegistryLike
} from './payload-codec';

const CREATION_PACKET_NAME = 'ZLinkFrameworkCreationRequest';

export function encodeFrameworkCreationPayload(
  payload: unknown,
  registry?: ZLinkSerializerRegistryLike | ReadonlyMap<string, ZLinkMessageSerializer>
): Buffer {
  const message = encodeFrameworkPayloadMessage(payload ?? null, registry);
  try {
    return encodeApplicationPayload({
      packetName: CREATION_PACKET_NAME,
      contentType: frameworkPayloadContentType(message),
      payload: message.data()
    });
  } finally {
    message.close();
  }
}

export function decodeFrameworkCreationPayload<T>(
  payload: Uint8Array,
  registry?: ZLinkSerializerRegistryLike | ReadonlyMap<string, ZLinkMessageSerializer>
): T {
  try {
    const application = decodeApplicationPayloadView(payload);
    if (application.packetName !== CREATION_PACKET_NAME) {
      throw new RangeError('Creation payload has an unexpected packet name.');
    }
    // The M6A view decoder returns a Buffer over the retained input storage.
    const message = RuntimeMessage.fromOwned(application.payload as Buffer);
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

import { createZlinkStreamProtobufEnvelopeCodec } from '@zlink-systems/framework-codec-protobuf';
import { bingoProtobufOptions } from './protobuf-codec';

export const bingoProtobuf = createZlinkStreamProtobufEnvelopeCodec(bingoProtobufOptions);

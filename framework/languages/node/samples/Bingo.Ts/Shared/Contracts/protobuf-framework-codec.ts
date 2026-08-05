import {
  createZlinkProtobufEnvelopeCodec
} from '@zlink-systems/framework-codec-protobuf/framework';
import { bingoProtobufOptions } from './protobuf-codec';

const bingoFrameworkProtobuf = createZlinkProtobufEnvelopeCodec(bingoProtobufOptions);

export { bingoFrameworkProtobuf };

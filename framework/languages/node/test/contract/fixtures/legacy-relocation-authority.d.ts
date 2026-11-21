import {
  ServiceRelocationAuthorityPayloadCodec,
  type ServiceRelocationPublication
} from '../../../packages/framework/src/runtime/foundation/service-relocation-runtime';
export class LegacyRelocationAuthorityFixture extends ServiceRelocationAuthorityPayloadCodec {
  publish(base: Uint8Array, publication: ServiceRelocationPublication): Buffer;
}

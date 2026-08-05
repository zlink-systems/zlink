import type { Type, ZLinkEncodedPayload } from '../Common';

export interface ZLinkMessageSerializer {
  serialize<T>(value: T): ZLinkEncodedPayload;
  deserialize<T>(payload: ZLinkEncodedPayload, type: Type<T>): T;
}

import { ZLinkEncodedPayload } from './ZLinkEncodedPayload';

export function borrowEncodedPayload(owner: object): Buffer | undefined {
  return owner instanceof ZLinkEncodedPayload
    ? (ZLinkEncodedPayload.prototype.view.call(owner) as Buffer)
    : undefined;
}

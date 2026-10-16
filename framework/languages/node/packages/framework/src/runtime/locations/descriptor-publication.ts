/** A descriptor Store write failed without a confirmed result (Location runtime section 5). */
export class ZLinkDescriptorPublicationUnconfirmedError extends Error {
  constructor(cause: unknown) {
    super(cause instanceof Error ? cause.message : String(cause), { cause });
    this.name = 'ZLinkDescriptorPublicationUnconfirmedError';
  }
}

import { ZLinkConfigurationException } from '../../contracts/Configuration/ConfigurationException';

/** A descriptor Store write failed without a confirmed result (Location runtime section 5). */
export class ZLinkDescriptorPublicationUnconfirmedError extends Error {
  constructor(cause: unknown) {
    super(cause instanceof Error ? cause.message : String(cause), { cause });
    this.name = 'ZLinkDescriptorPublicationUnconfirmedError';
  }
}

export class ZLinkMeshNodeDescriptorConflictException extends ZLinkConfigurationException {
  constructor(message: string) {
    super(message);
    this.name = 'ZLinkMeshNodeDescriptorConflictException';
  }
}

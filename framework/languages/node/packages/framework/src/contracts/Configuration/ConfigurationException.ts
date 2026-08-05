export class ZLinkConfigurationException extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'ZLinkConfigurationException';
  }
}

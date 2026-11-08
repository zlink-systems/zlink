export interface ZLinkEtcdClientIdentity {
  readonly certificatePem: string;
  readonly keyPem: string;
}
export interface ZLinkEtcdTlsOptions {
  readonly trustedCaPem?: string;
  readonly clientIdentity?: ZLinkEtcdClientIdentity;
}
export interface ZLinkEtcdCredentials {
  readonly user: string;
  readonly password: string;
}
export interface ZLinkEtcdLocationOptions {
  readonly endpoints: readonly string[];
  readonly tls?: ZLinkEtcdTlsOptions;
  readonly credentials?: ZLinkEtcdCredentials;
  readonly namespace: string;
  readonly operationTimeoutMs?: number;
}

import * as grpc from '@grpc/grpc-js';
import * as loader from '@grpc/proto-loader';
import { existsSync } from 'node:fs';
import { resolve } from 'node:path';
import { getCACertificates } from 'node:tls';
import { DEFAULT_OPERATION_TIMEOUT_MS, EtcdOptions, validateOptions } from './options';

export type ErrorKind = 'compaction' | 'nospace' | 'auth' | 'storeFailure';
export class EtcdCallError extends Error {
  constructor(readonly kind: ErrorKind, readonly code: number, readonly description: string, readonly endpoint: string) {
    super(`etcd ${kind} (gRPC ${code})`);
  }
}

export function classifyError(code: number, description: string): ErrorKind {
  if (code === grpc.status.OUT_OF_RANGE && description.includes('required revision has been compacted')) return 'compaction';
  if (code === grpc.status.RESOURCE_EXHAUSTED && description.includes('database space exceeded')) return 'nospace';
  if ((code === grpc.status.UNAUTHENTICATED && description.includes('invalid auth token')) ||
      (code === grpc.status.INVALID_ARGUMENT && description.includes('revision of auth store is old'))) return 'auth';
  return 'storeFailure';
}

export const CHANNEL_OPTIONS: grpc.ChannelOptions = {
  'grpc.enable_retries': 0,
  'grpc.service_config_disable_resolution': 1,
  'grpc.max_send_message_length': 16 * 1024 * 1024,
  'grpc.max_receive_message_length': 8 * 1024 * 1024
};

export function protoDefinition(): loader.PackageDefinition {
  const packed = resolve(__dirname, '../proto/v3.6.16');
  const root = existsSync(packed) ? packed : resolve(__dirname, '../../../../../../runtime/protocol/etcd/v3.6.16');
  return loader.loadSync(resolve(root, 'etcd/api/etcdserverpb/rpc.proto'), {
    keepCase: true, longs: String, bytes: Buffer, defaults: true, includeDirs: [root]
  });
}

export interface Operation { readonly deadline: Date; readonly signal?: AbortSignal }
export interface KeyValue { key: Buffer; value: Buffer; mod_revision: string; create_revision: string; version: string }
export interface RangeResponse { kvs: KeyValue[]; more: boolean; header: { revision: string; cluster_id: string } }
export interface TxnResponse { succeeded: boolean; header: { revision: string; cluster_id: string } }
type Services = Record<'KV' | 'Auth', grpc.ServiceClientConstructor>;

export class EtcdClient {
  private readonly channels: grpc.Channel[];
  private readonly clients: Record<'KV' | 'Auth', grpc.Client>[];
  private readonly services: Services;
  private endpoint = 0;
  private token: string | undefined;

  constructor(readonly options: EtcdOptions) {
    const endpoints = validateOptions(options);
    this.services = (grpc.loadPackageDefinition(protoDefinition()) as unknown as { etcdserverpb: Services }).etcdserverpb;
    const tls = options.tls;
    const identity = tls?.clientIdentity;
    const credentials = endpoints[0].protocol === 'https:'
      ? grpc.credentials.createSsl(Buffer.from(tls?.trustedCaPem ?? getCACertificates('system').join('\n')),
        identity === undefined ? undefined : Buffer.from(identity.keyPem), identity === undefined ? undefined : Buffer.from(identity.certificatePem))
      : grpc.credentials.createInsecure();
    this.channels = endpoints.map(endpoint => new grpc.Channel(`${endpoint.hostname}:${endpoint.port === '' ? (endpoint.protocol === 'https:' ? '443' : '80') : endpoint.port}`, credentials, CHANNEL_OPTIONS));
    this.clients = this.channels.map(channel => ({
      KV: new this.services.KV('', credentials, { channelOverride: channel }),
      Auth: new this.services.Auth('', credentials, { channelOverride: channel })
    }));
  }

  operation(signal?: AbortSignal): Operation { return { deadline: new Date(Date.now() + (this.options.operationTimeoutMs ?? DEFAULT_OPERATION_TIMEOUT_MS)), signal }; }

  private async authenticate(operation: Operation): Promise<void> {
    const credentials = this.options.credentials;
    if (credentials === undefined) return;
    const response = await this.invoke<{ token: string }>('Auth', 'authenticate', { name: credentials.user, password: credentials.password }, operation, false);
    this.token = response.token;
  }

  async rpc<T = RangeResponse>(service: 'KV' | 'Auth', method: string, request: object, operation: Operation): Promise<T> {
    if (this.options.credentials !== undefined && this.token === undefined) await this.authenticate(operation);
    try { return await this.invoke<T>(service, method, request, operation, true); }
    catch (error) {
      if (!(error instanceof EtcdCallError) || error.kind !== 'auth' || this.options.credentials === undefined) throw error;
      await this.authenticate(operation);
      return this.invoke<T>(service, method, request, operation, true);
    }
  }

  private invoke<T>(service: 'KV' | 'Auth', method: string, request: object, operation: Operation, authenticated: boolean): Promise<T> {
    if (operation.signal?.aborted === true) return Promise.reject(new Error('Store operation stopped'));
    const index = this.endpoint;
    const descriptor = Object.entries(this.services[service].service).find(([name]) => name.toLowerCase() === method.toLowerCase())?.[1];
    if (descriptor === undefined) return Promise.reject(new TypeError('Unknown RPC'));
    const metadata = new grpc.Metadata();
    if (authenticated && this.token !== undefined) metadata.set('token', this.token);
    return new Promise((accept, reject) => {
      this.clients[index][service].makeUnaryRequest(descriptor.path, descriptor.requestSerialize, descriptor.responseDeserialize,
        request, metadata, { deadline: operation.deadline }, (error: grpc.ServiceError | null, response: T | undefined) => {
          if (error !== null) {
            if ((error.code === grpc.status.UNAVAILABLE || error.code === grpc.status.DEADLINE_EXCEEDED) && this.endpoint === index) this.endpoint = (index + 1) % this.channels.length;
            reject(new EtcdCallError(classifyError(error.code, error.details), error.code, error.details, this.options.endpoints[index]));
          } else if (response === undefined) reject(new Error('Missing RPC response'));
          else accept(response);
        });
    });
  }

  close(): void { for (const channel of this.channels) channel.close(); }
}

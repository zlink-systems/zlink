export interface ServiceSessionBindingRetainedDelivery {
  deliver(): Promise<boolean>;
  fail(error: unknown): void;
}

export interface ServiceSessionBindingAdmissionClaim {
  readonly actorId: string;
  readonly objectGeneration: bigint;
  readonly actorNodeRid: string;
  readonly actorNodeGeneration: bigint;
  readonly authorityOwnerGeneration: bigint;
  readonly ownerLeaseGeneration: bigint;
  readonly producerNodeRid: string;
  readonly producerNodeGeneration: bigint;
  readonly sessionIdentity: string;
  readonly bindingGeneration: bigint;
}

export type ServiceSessionBindingAdmissionResult =
  | 'passThrough'
  | 'retained'
  | 'rejected';

export interface ServiceSessionBindingIngressPort {
  retainOutbound(
    claim: ServiceSessionBindingAdmissionClaim,
    delivery: ServiceSessionBindingRetainedDelivery
  ): ServiceSessionBindingAdmissionResult;
  clearOutbound(actorId: string, error: unknown): void;
}

const ports = new WeakMap<object, ServiceSessionBindingIngressPort>();

export function registerServiceSessionBindingIngressPort(
  service: object,
  port: ServiceSessionBindingIngressPort
): void {
  ports.set(service, port);
}

export function serviceSessionBindingIngressPortIfRegistered(
  service: object
): ServiceSessionBindingIngressPort | undefined {
  return ports.get(service);
}

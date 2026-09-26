export interface ServiceSessionBindingRetainedDelivery {
  deliver(): Promise<boolean>;
  fail(error: unknown): void;
}

export type ServiceSessionBindingAdmissionResult = 'passThrough' | 'retained' | 'rejected';

export interface ServiceSessionBindingIngressPort {
  actorSlot(actorId: string, sessionRid: string): Promise<number | undefined>;
  /**
   * Holds a current-binding push while its relocation seal is open
   * (Session–Actor binding §8.1). The caller has already decided that the push
   * names the current binding.
   */
  retainOutbound(
    actorId: string,
    delivery: ServiceSessionBindingRetainedDelivery
  ): Promise<ServiceSessionBindingAdmissionResult>;
  clearOutbound(actorId: string, error: unknown): Promise<void>;
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

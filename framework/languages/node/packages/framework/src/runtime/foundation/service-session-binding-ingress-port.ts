export interface ServiceSessionBindingIngressPort {
  actorSlot(actorId: string, sessionRid: string): Promise<number | undefined>;
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

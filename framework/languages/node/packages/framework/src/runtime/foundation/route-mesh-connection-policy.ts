import type {
  ServiceNodeDescriptor,
  ServiceObjectRole
} from './service-topology-registry';

export function routeMeshConnectionNotRequired(
  localObjectRole: ServiceObjectRole,
  localHasServerChannel: boolean,
  remoteObjectRole: ServiceObjectRole,
  remoteHasServerChannel: boolean
): boolean {
  return localObjectRole === 'client'
    && !localHasServerChannel
    && remoteObjectRole === 'client'
    && !remoteHasServerChannel;
}

export function descriptorConnectionNotRequired(
  local: ServiceNodeDescriptor,
  remote: ServiceNodeDescriptor
): boolean {
  return routeMeshConnectionNotRequired(
    local.objectRole,
    local.channels.length > 0,
    remote.objectRole,
    remote.channels.length > 0
  );
}

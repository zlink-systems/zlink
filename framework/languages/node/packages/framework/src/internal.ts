/** Test and companion-package integration surface. It is not exported by package.json. */
export * from './index';
export { requireValidSendTimeoutMs } from './contracts/Configuration/SendTimeoutValidation';
export * from './runtime/diagnostics';
export * from './runtime/host';
export { ZLinkActorPlacementCoordinator } from './runtime/host/actor-placement-coordinator';
export { ZLinkActorTransferRuntime } from './runtime/host/actor-transfer-runtime';
export {
  ApplicationJobQueue,
  ApplicationJobQueuePermit,
  nodeEffectiveProcessorCount,
  resolveApplicationJobQueueConfiguration
} from './runtime/host/application-job-queue';
export { ZLinkLocationRuntimeOwner } from './runtime/host/location-runtime-owner';
export {
  relocationFailedFailureCode,
  ZLinkHostServiceRelocationRuntime
} from './runtime/host/service-relocation-host-runtime';
export * from './runtime/admission';
export * from './runtime/activation-admission';
export * from './runtime/streams';
export * from './runtime/streams/protocol';
export * from './runtime/actors';
export * from './runtime/spots';
export * from './runtime/workers';
export * from './runtime/configuration';
export * from './runtime/locations';
export * from './runtime/locations/key-codec';
export * from './runtime/locations/canonical-codec';
export {
  ZLinkLocationAutoConnectType,
  ZLinkLocationKind,
  ZLinkRouteKind
} from './contracts/Locations/Values';
export { ZLinkLocationWriteIntent, ZLinkLocationWriteStatus } from './contracts/Locations/Writes';
export * from './runtime/channels';
export { ZLinkChannelRuntimeLifecycle } from './runtime/channels/channel-runtime-lifecycle';
export { ZLinkChannelSocketRegistry } from './runtime/channels/channel-socket-registry';
export { ZLinkChannelDispatchPipeline } from './runtime/channels/channel-dispatch-pipeline';
export * from './runtime/backend';
export * from './runtime/codecs';
export * from './runtime/execution';
export { ZLinkSerialExecutionQueue } from './runtime/execution/serial-execution-queue';
export * from './runtime/handlers';
export * from './runtime/messaging';
export * from './runtime/foundation';
export * from './runtime/foundation/service-authority-payload-codec';
export * from './runtime/framework-errors-internal';
export * from './contracts/Configuration/Registration';

export * from './contracts/Locations/Stores';
export { ZLINK_RELOCATION_MAX_BLOB_REFERENCE_BYTES } from './contracts/Configuration/InternalDefaults';
export { ZLINK_MAX_STABLE_TYPE_BYTES } from './contracts/Common/CoreTypes';
export {
  MAX_LISTENER_PORT,
  ZLINK_MAX_PUBLIC_WEIGHT,
  ZLINK_MAX_CAPACITY,
  isValidPublicWeight,
  isValidPositiveCapacity,
  isValidListenerPort,
  isValidCapacity,
  requirePublicWeight
} from './contracts/Configuration/RegistrationBuilderPolicy';

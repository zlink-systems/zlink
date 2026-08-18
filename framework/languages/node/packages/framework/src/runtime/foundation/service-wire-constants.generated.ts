// Generated from service-wire-v1.schema.json. Do not edit.
export const SERVICE_WIRE_MAGIC = [90, 77] as const;
export const SERVICE_WIRE_MAJOR = 1 as const;
export const SERVICE_WIRE_REQUIRED_CAPABILITY = "framework-service-v13" as const;
export const SERVICE_FRAMEWORK_MULTIPART_PACKET_NAME = "ZLinkFrameworkMultipart" as const;
export const SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE = "application/x-zlink-multipart" as const;
export const ServiceWireCommand = {
  hello: 1,
  admit: 2,
  reject: 3,
  update: 4,
  livenessProbe: 5,
  livenessAck: 6,
  nodeSend: 16,
  nodeRequest: 17,
  channelSend: 18,
  channelRequest: 19,
  reply: 20,
  spotSend: 21,
  spotRequest: 22,
  logicalMulticast: 23,
  actorSend: 24,
  actorRequest: 25,
  actorLookup: 26,
  actorDestroy: 27,
  actorJoin: 28,
  actorLeft: 29,
  relocationReady: 30,
  relocationData: 31,
  replyRelay: 33,
  relocationCutover: 34,
  boundSessionSend: 36,
  actorJoined: 37,
  boundSessionBind: 38,
  instanceSpot: 39,
  relocationPrepare: 40,
  sessionRelocationSeal: 42,
  sessionRelocationSealed: 43,
  sessionRelocationRoute: 44,
  replyRelayAck: 46,
  userSpotCreate: 47,
  userSpotClose: 48,
  actorCreate: 49,
  messageFollow: 50,
  boundSessionReplaced: 51,
  relocationState: 52,
  relocationFailed: 53,
} as const;
export const ServiceWireFlag = {
  metadata: 1,
  boundSession: 2,
  sourceSpotId: 4,
  extension: 8,
} as const;
export const ServiceWireFrameworkErrorCode = {
  none: 0,
  actorRouteNotFound: 1,
  actorCreateFailed: 2,
  actorAlreadyExists: 3,
  actorTypeMismatch: 4,
  spotCreateFailed: 5,
  spotRouteNotFound: 6,
  spotTypeMismatch: 7,
  actorSessionNotBound: 8,
  handlerNotFound: 9,
  routeHandlerNotFound: 10,
  actorDispatchHandlerNotFound: 11,
  payloadDecodeFailed: 12,
  routeNotConnected: 13,
  requestTargetNotFound: 14,
  requestRejected: 15,
  requestProtocolError: 16,
  requestFailed: 17,
  workerQueueFull: 18,
  workerTimedOut: 19,
  workerFailed: 20,
  actorLocationStale: 21,
  actorCreateRejected: 22,
  spotGenerationStale: 33,
  spotMoving: 34,
  relocationDataLost: 35,
} as const;
export const ServiceWireBoundaryTerminalResults = [101, 103, 108, 109, 110, 111, 112, 113] as const;
export const ServiceWireExactTerminalByFailureCode = {
  1: 102, // actorRouteNotFound
  2: 105, // actorCreateFailed
  3: 107, // actorAlreadyExists
  4: 107, // actorTypeMismatch
  5: 105, // spotCreateFailed
  6: 102, // spotRouteNotFound
  7: 107, // spotTypeMismatch
  8: 102, // actorSessionNotBound
  9: 102, // handlerNotFound
  10: 102, // routeHandlerNotFound
  11: 102, // actorDispatchHandlerNotFound
  12: 104, // payloadDecodeFailed
  13: 105, // routeNotConnected
  14: 102, // requestTargetNotFound
  15: 106, // requestRejected
  16: 104, // requestProtocolError
  17: 105, // requestFailed
  18: 106, // workerQueueFull
  19: 105, // workerTimedOut
  20: 105, // workerFailed
  21: 107, // actorLocationStale
  22: 106, // actorCreateRejected
  33: 107, // spotGenerationStale
  34: 107, // spotMoving
  35: 105, // relocationDataLost
} as const;
// Schema terminal-failure-integrity: success is ok+none, boundary terminals
// carry none, typed failures must match their exact terminal, and an unknown
// failure code is a protocol error before dispatch.
export const isValidServiceWireTerminalFailure = (
  terminal: number,
  failureCode: number
): boolean => {
  if (terminal === 0) {
    return failureCode === 0;
  }
  if ((ServiceWireBoundaryTerminalResults as readonly number[]).includes(terminal)) {
    return failureCode === 0;
  }
  if (failureCode === 0) {
    return false;
  }
  const expected = (ServiceWireExactTerminalByFailureCode as Record<number, number>)[failureCode];
  return expected !== undefined && expected === terminal;
};

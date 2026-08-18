"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.isValidServiceWireTerminalFailure = exports.ServiceWireExactTerminalByFailureCode = exports.ServiceWireBoundaryTerminalResults = exports.ServiceWireFrameworkErrorCode = exports.ServiceWireFlag = exports.ServiceWireCommand = exports.SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE = exports.SERVICE_FRAMEWORK_MULTIPART_PACKET_NAME = exports.SERVICE_WIRE_REQUIRED_CAPABILITY = exports.SERVICE_WIRE_MAJOR = exports.SERVICE_WIRE_MAGIC = void 0;
// Generated from service-wire-v1.schema.json. Do not edit.
exports.SERVICE_WIRE_MAGIC = [90, 77];
exports.SERVICE_WIRE_MAJOR = 1;
exports.SERVICE_WIRE_REQUIRED_CAPABILITY = "framework-service-v13";
exports.SERVICE_FRAMEWORK_MULTIPART_PACKET_NAME = "ZLinkFrameworkMultipart";
exports.SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE = "application/x-zlink-multipart";
exports.ServiceWireCommand = {
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
};
exports.ServiceWireFlag = {
    metadata: 1,
    boundSession: 2,
    sourceSpotId: 4,
    extension: 8,
};
exports.ServiceWireFrameworkErrorCode = {
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
};
exports.ServiceWireBoundaryTerminalResults = [101, 103, 108, 109, 110, 111, 112, 113];
exports.ServiceWireExactTerminalByFailureCode = {
    1: 102,
    2: 105,
    3: 107,
    4: 107,
    5: 105,
    6: 102,
    7: 107,
    8: 102,
    9: 102,
    10: 102,
    11: 102,
    12: 104,
    13: 105,
    14: 102,
    15: 106,
    16: 104,
    17: 105,
    18: 106,
    19: 105,
    20: 105,
    21: 107,
    22: 106,
    33: 107,
    34: 107,
    35: 105,
};
// Schema terminal-failure-integrity: success is ok+none, boundary terminals
// carry none, typed failures must match their exact terminal, and an unknown
// failure code is a protocol error before dispatch.
const isValidServiceWireTerminalFailure = (terminal, failureCode) => {
    if (terminal === 0) {
        return failureCode === 0;
    }
    if (exports.ServiceWireBoundaryTerminalResults.includes(terminal)) {
        return failureCode === 0;
    }
    if (failureCode === 0) {
        return false;
    }
    const expected = exports.ServiceWireExactTerminalByFailureCode[failureCode];
    return expected !== undefined && expected === terminal;
};
exports.isValidServiceWireTerminalFailure = isValidServiceWireTerminalFailure;

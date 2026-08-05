// Generated from service-wire-v1.schema.json. Do not edit.
namespace Systems.Zlink.Framework.Runtime.Protocol;

internal static class ServiceWireConstants
{
    internal const byte Magic0 = 90;
    internal const byte Magic1 = 77;
    internal const byte WireMajor = 1;
    internal const string RequiredCapability = "framework-service-v11";
    internal const string FrameworkMultipartPacketName = "ZLinkFrameworkMultipart";
    internal const string FrameworkMultipartContentType = "application/x-zlink-multipart";
    internal enum Command : byte
    {
        Hello = 1,
        Admit = 2,
        Reject = 3,
        Update = 4,
        LivenessProbe = 5,
        LivenessAck = 6,
        NodeSend = 16,
        NodeRequest = 17,
        ChannelSend = 18,
        ChannelRequest = 19,
        Reply = 20,
        SpotSend = 21,
        SpotRequest = 22,
        LogicalMulticast = 23,
        ActorSend = 24,
        ActorRequest = 25,
        ActorLookup = 26,
        ActorDestroy = 27,
        ActorJoin = 28,
        ActorLeft = 29,
        RelocationReady = 30,
        RelocationData = 31,
        RelocationAck = 32,
        ReplyRelay = 33,
        RelocationSeal = 34,
        RelocationComplete = 35,
        BoundSessionSend = 36,
        ActorJoined = 37,
        BoundSessionBind = 38,
        InstanceSpot = 39,
        RelocationPrepare = 40,
        RelocationReserved = 41,
        SessionRelocationSeal = 42,
        SessionRelocationSealed = 43,
        SessionRelocationRoute = 44,
        SessionRelocationRouted = 45,
        ReplyRelayAck = 46,
        UserSpotCreate = 47,
        UserSpotClose = 48,
        ActorCreate = 49,
        MessageFollow = 50,
    }
    [System.Flags]
    internal enum Flag : byte
    {
        None = 0,
        Metadata = 1,
        BoundSession = 2,
        SourceSpotId = 4,
        Extension = 8,
    }
    internal enum FrameworkErrorCode : uint
    {
        None = 0,
        ActorRouteNotFound = 1,
        ActorCreateFailed = 2,
        ActorAlreadyExists = 3,
        ActorTypeMismatch = 4,
        SpotCreateFailed = 5,
        SpotRouteNotFound = 6,
        SpotTypeMismatch = 7,
        ActorSessionNotBound = 8,
        HandlerNotFound = 9,
        RouteHandlerNotFound = 10,
        ActorDispatchHandlerNotFound = 11,
        PayloadDecodeFailed = 12,
        RouteNotConnected = 13,
        RequestTargetNotFound = 14,
        RequestRejected = 15,
        RequestProtocolError = 16,
        RequestFailed = 17,
        WorkerQueueFull = 18,
        WorkerTimedOut = 19,
        WorkerFailed = 20,
        ActorLocationStale = 21,
        ActorCreateRejected = 22,
        SpotGenerationStale = 33,
        SpotMoving = 34,
        RelocationDataLost = 35,
    }
}

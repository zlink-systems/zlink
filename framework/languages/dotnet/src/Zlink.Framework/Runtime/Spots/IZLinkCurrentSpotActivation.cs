namespace Zlink.Framework.Runtime.Spots;

internal interface IZLinkCurrentSpotActivation
{
    void EnsureOperationAllowed();

    string ChannelName { get; }

    string SpotId { get; }

    ZLinkUserSpotExecutionMode ExecutionMode { get; }

    TimeSpan DefaultRequestTimeout { get; }

    ZLinkCodecRegistryBuilder Codecs { get; }

    ZLinkMessageFlowTracer Flow { get; }

    IZLinkRuntimeFailureReporter ErrorSink { get; }

    IZLinkSpotOutbound Outbound { get; }

    ZLinkSpotOutboundEndpoint OutboundEndpoint { get; }
}

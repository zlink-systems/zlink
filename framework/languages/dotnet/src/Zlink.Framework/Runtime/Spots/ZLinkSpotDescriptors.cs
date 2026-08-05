namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkSpotDescriptor
{
    public required Type HandlerType { get; init; }

    public required Type SpotType { get; init; }

    public required Type MessageType { get; init; }

    public required ZLinkHandlerMethodInvoker Invoker { get; init; }

    public required string MessageName { get; init; }

    public Type? ReplyType { get; init; }

    public bool IsAttributed { get; init; }

    public bool PassCancellationToken { get; init; }

    public bool IsRequest => ReplyType is not null;
}

internal sealed class ZLinkSpotSubscriptionDescriptor
{
    public required string ChannelName { get; init; }

    public required string Topic { get; init; }

    public required Type HandlerType { get; init; }

    public required Type SpotType { get; init; }

    public required Type MessageType { get; init; }

    public required ZLinkHandlerMethodInvoker Invoker { get; init; }

    public required string MessageName { get; init; }

    public bool IsAttributed { get; init; }

    public bool PassCancellationToken { get; init; }
}

internal sealed class ZLinkSpotTimerDescriptor
{
    public required string Name { get; init; }

    public required TimeSpan Period { get; init; }

    public required Type HandlerType { get; init; }

    public required Type SpotType { get; init; }

    public required ZLinkHandlerMethodInvoker Invoker { get; init; }
}

internal sealed class ZLinkSpotActorJoinDescriptor
{
    public required Type HandlerType { get; init; }

    public required Type SpotType { get; init; }

    public required Type ActorType { get; init; }

    public required ZLinkHandlerMethodInvoker Invoker { get; init; }

    public bool PassSpotArgument { get; init; } = true;
}

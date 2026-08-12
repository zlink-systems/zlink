using RegistrationCodec.Server.JsonOnlyPeer.Infrastructure;
using RegistrationCodec.Shared;
using Zlink.Framework.Contracts.Handlers;

namespace RegistrationCodec.Server.JsonOnlyPeer.Handlers;

internal sealed class JsonEchoRequestHandler(EvidenceStore evidence)
    : IZLinkRequestHandler<JsonEchoReq, EchoRes>
{
    public ValueTask<EchoRes> HandleAsync(JsonEchoReq request, IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"codec-request|codec=json|value={request.Value}|content={context.ContentType}");
        return ValueTask.FromResult(new EchoRes($"echo:{request.Value}", context.ContentType ?? "<null>"));
    }
}

internal sealed class JsonEchoCommandHandler(EvidenceStore evidence)
    : IZLinkSendHandler<JsonEchoMsg>
{
    public ValueTask HandleAsync(JsonEchoMsg message, IZLinkMessageContext context, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add(
            $"codec-command|codec=json|id={message.CommandId}|value={message.Value}|content={context.ContentType}");
        return ValueTask.CompletedTask;
    }
}

internal sealed class ProtobufEchoRequestHandler(EvidenceStore evidence)
    : IZLinkRequestHandler<ProtobufEchoReq, ProtobufEchoRes>
{
    public ValueTask<ProtobufEchoRes> HandleAsync(ProtobufEchoReq request, IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"codec-request|codec=protobuf|value={request.Value}|content={context.ContentType}");
        return ValueTask.FromResult(
            new ProtobufEchoRes { Value = $"echo:{request.Value}|content:{context.ContentType}" });
    }
}

internal sealed class ProtobufEchoCommandHandler(EvidenceStore evidence)
    : IZLinkSendHandler<ProtobufEchoMsg>
{
    public ValueTask HandleAsync(
        ProtobufEchoMsg message,
        IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"codec-command|codec=protobuf|value={message.Value}|content={context.ContentType}");
        return ValueTask.CompletedTask;
    }
}

internal sealed class MessagePackEchoRequestHandler(EvidenceStore evidence)
    : IZLinkRequestHandler<PackedEchoReq, PackedEchoRes>
{
    public ValueTask<PackedEchoRes> HandleAsync(PackedEchoReq request, IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"codec-request|codec=msgpack|value={request.Value}|content={context.ContentType}");
        return ValueTask.FromResult(new PackedEchoRes
            { Value = $"echo:{request.Value}|content:{context.ContentType}" });
    }
}

internal sealed class MessagePackEchoCommandHandler(EvidenceStore evidence)
    : IZLinkSendHandler<PackedEchoMsg>
{
    public ValueTask HandleAsync(PackedEchoMsg message, IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add(
            $"codec-command|codec=msgpack|id={message.CommandId}|value={message.Value}|content={context.ContentType}");
        return ValueTask.CompletedTask;
    }
}

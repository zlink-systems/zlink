using Google.Protobuf.WellKnownTypes;
using RegistrationCodec.Server.Main.Infrastructure;
using RegistrationCodec.Shared;
using Zlink.Framework.Contracts.Handlers;

namespace RegistrationCodec.Server.Main.Handlers;

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
    : IZLinkRequestHandler<StringValue, StringValue>
{
    public ValueTask<StringValue> HandleAsync(StringValue request, IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"codec-request|codec=protobuf|value={request.Value}|content={context.ContentType}");
        return ValueTask.FromResult(new StringValue { Value = $"echo:{request.Value}|content:{context.ContentType}" });
    }
}

internal sealed class ProtobufEchoCommandHandler(EvidenceStore evidence)
    : IZLinkSendHandler<StringValue>
{
    public ValueTask HandleAsync(StringValue message, IZLinkMessageContext context, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"codec-command|codec=protobuf|value={message.Value}|content={context.ContentType}");
        return ValueTask.CompletedTask;
    }
}

internal sealed class MessagePackEchoRequestHandler(EvidenceStore evidence)
    : IZLinkRequestHandler<PackedEchoReq, PackedEchoReq>
{
    public ValueTask<PackedEchoReq> HandleAsync(PackedEchoReq request, IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"codec-request|codec=msgpack|value={request.Value}|content={context.ContentType}");
        return ValueTask.FromResult(new PackedEchoReq
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

internal sealed class JsonGoldenRequestHandler(EvidenceStore evidence)
    : IZLinkRequestHandler<JsonGoldenReq, JsonGoldenRes>
{
    public ValueTask<JsonGoldenRes> HandleAsync(
        JsonGoldenReq request,
        IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add(
            $"codec-request|codec=json|value=golden|status={request.Status}|balance={request.Balance}|score={request.Score}|payload={Convert.ToBase64String(request.Payload)}");
        return ValueTask.FromResult(new JsonGoldenRes(
            request.DisplayName,
            request.Status,
            request.Balance,
            request.Payload,
            request.Score,
            request.Ratio,
            request.OptionalNote,
            context.ContentType ?? "<null>"));
    }
}

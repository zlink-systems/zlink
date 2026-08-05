using RegistrationCodec.Server.Main.Infrastructure;
using RegistrationCodec.Shared;
using Zlink.Framework.Contracts.Handlers;

namespace RegistrationCodec.Server.Main.Handlers;

[ZLinkHandlerGroup("auto")]
internal sealed class EchoAutoRequestHandler(EvidenceStore evidence)
    : IZLinkRequestHandler<EchoAutoReq, EchoRes>
{
    public ValueTask<EchoRes> HandleAsync(EchoAutoReq request, IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"echo-request|variant=auto|value={request.Value}|content={context.ContentType}");
        return ValueTask.FromResult(new EchoRes($"echo:{request.Value}", context.ContentType ?? "<null>"));
    }
}

[ZLinkHandlerGroup("auto")]
internal sealed class EchoAutoCommandHandler(EvidenceStore evidence)
    : IZLinkSendHandler<EchoAutoMsg>
{
    public ValueTask HandleAsync(EchoAutoMsg message, IZLinkMessageContext context, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add(
            $"echo-command|variant=auto|id={message.CommandId}|value={message.Value}|content={context.ContentType}");
        return ValueTask.CompletedTask;
    }
}

[ZLinkHandlerGroup("attr")]
internal sealed class AttributeHandlers(EvidenceStore evidence)
    : IZLinkRequestHandler<EchoAttrReq, EchoRes>,
      IZLinkSendHandler<EchoAttrMsg>
{
    [ZLinkRequest(PacketName = "EchoAttr")]
    public EchoRes Request(EchoAttrReq request, IZLinkMessageContext context, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"echo-request|variant=attr|value={request.Value}|content={context.ContentType}");
        return new EchoRes($"echo:{request.Value}", context.ContentType ?? "<null>");
    }

    [ZLinkSend(PacketName = "EchoAttrMsg")]
    public ValueTask Send(EchoAttrMsg message, IZLinkMessageContext context, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add(
            $"echo-command|variant=attr|id={message.CommandId}|value={message.Value}|content={context.ContentType}");
        return ValueTask.CompletedTask;
    }

    ValueTask<EchoRes> IZLinkRequestHandler<EchoAttrReq, EchoRes>.HandleAsync(
        EchoAttrReq request,
        IZLinkMessageContext context,
        CancellationToken cancellationToken) =>
        ValueTask.FromResult(Request(request, context, cancellationToken));

    ValueTask IZLinkSendHandler<EchoAttrMsg>.HandleAsync(
        EchoAttrMsg message,
        IZLinkMessageContext context,
        CancellationToken cancellationToken) =>
        Send(message, context, cancellationToken);
}

internal sealed class EchoManualRequestHandler(EvidenceStore evidence)
    : IZLinkRequestHandler<EchoManualReq, EchoRes>
{
    public ValueTask<EchoRes> HandleAsync(EchoManualReq request, IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"echo-request|variant=manual|value={request.Value}|content={context.ContentType}");
        return ValueTask.FromResult(new EchoRes($"echo:{request.Value}", context.ContentType ?? "<null>"));
    }
}

internal sealed class EchoManualCommandHandler(EvidenceStore evidence)
    : IZLinkSendHandler<EchoManualMsg>
{
    public ValueTask HandleAsync(EchoManualMsg message, IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add(
            $"echo-command|variant=manual|id={message.CommandId}|value={message.Value}|content={context.ContentType}");
        return ValueTask.CompletedTask;
    }
}

internal sealed class DuplicateEchoRequestHandler
    : IZLinkRequestHandler<EchoManualReq, EchoRes>
{
    public ValueTask<EchoRes> HandleAsync(EchoManualReq request, IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        _ = context;
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(new EchoRes(request.Value, "duplicate"));
    }
}

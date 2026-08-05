using RegistrationCodec.Server.JsonOnlyPeer.Infrastructure;
using RegistrationCodec.Shared;
using Zlink.Framework.Contracts.Handlers;

namespace RegistrationCodec.Server.JsonOnlyPeer.Handlers;

internal sealed class DiEchoRequestHandler(
    EvidenceStore evidence,
    SingletonProbe singleton,
    ScopedProbe scoped)
    : IZLinkRequestHandler<EchoDiReq, EchoRes>
{
    public ValueTask<EchoRes> HandleAsync(EchoDiReq request, IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add(
            $"di|value={request.Value}|singleton={singleton.Id}|scoped={scoped.Id}|disposed={ScopedProbe.DisposedCount}");
        return ValueTask.FromResult(new EchoRes($"echo:{request.Value}", context.ContentType ?? "<null>"));
    }
}

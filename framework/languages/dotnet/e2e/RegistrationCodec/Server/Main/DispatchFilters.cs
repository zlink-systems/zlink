using RegistrationCodec.Server.Main.Infrastructure;
using Zlink.Framework.Contracts.Handlers;

namespace RegistrationCodec.Server.Main;

internal sealed class FirstFilter(EvidenceStore evidence) : IZLinkHandlerFilter
{
    public async ValueTask InvokeAsync(
        IZLinkHandlerFilterContext context,
        ZLinkHandlerFilterNext next,
        CancellationToken cancellationToken)
    {
        evidence.Add($"filter|name=first|phase=before|packet={context.PacketName}");
        await next();
        evidence.Add($"filter|name=first|phase=after|packet={context.PacketName}");
    }
}

internal sealed class SecondFilter(EvidenceStore evidence) : IZLinkHandlerFilter
{
    public async ValueTask InvokeAsync(
        IZLinkHandlerFilterContext context,
        ZLinkHandlerFilterNext next,
        CancellationToken cancellationToken)
    {
        evidence.Add($"filter|name=second|phase=before|packet={context.PacketName}");
        await next();
        evidence.Add($"filter|name=second|phase=after|packet={context.PacketName}");
    }
}

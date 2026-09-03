using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.Runtime.Streams;

internal sealed class ZLinkStreamInboundFrame(
    Message header,
    Message payload) : IDisposable
{
    internal Message? Header { get; private set; } = header;
    internal Message? Payload { get; private set; } = payload;

    internal ZLinkApplicationJobQueueLease? ApplicationJobAdmission { get; set; }

    internal void Detach()
    {
        Header = null;
        Payload = null;
        ApplicationJobAdmission = null;
    }

    public void Dispose()
    {
        Header?.Dispose();
        Payload?.Dispose();
        Header = null;
        Payload = null;
        ApplicationJobAdmission?.Dispose();
        ApplicationJobAdmission = null;
    }
}

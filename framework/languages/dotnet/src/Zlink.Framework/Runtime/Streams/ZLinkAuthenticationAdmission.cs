using System.Globalization;
using Zlink.Framework.Runtime.Configuration;
using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.Runtime.Streams;

internal sealed class ZLinkAuthenticationAdmission(ZLinkAuthenticationAdmissionOptions options)
{
    private int _inFlight;

    internal int InFlight => Volatile.Read(ref _inFlight);

    internal ZlinkStreamMetadata RetryMetadata { get; } =
        ZlinkStreamMetadata.Empty.With(
            Systems
                .Zlink
                .Stream
                .Connector
                .Runtime
                .ZlinkStreamReceiveDispatcher
                .RetryAfterMetadataKey,
            (options.RetryDelay.Ticks / TimeSpan.TicksPerMillisecond).ToString(
                CultureInfo.InvariantCulture
            )
        );

    internal bool TryAdmit(ZlinkStreamHeader header, out Lease? lease)
    {
        lease = null;
        if (
            header.Kind != ZlinkStreamMessageKind.Request
            || !string.Equals(header.Name, options.PacketName, StringComparison.Ordinal)
        )
            return true;

        int current;
        do
        {
            current = InFlight;
            if (current >= options.Limit)
            {
                ZLinkRuntimeMetrics.RecordAuthenticationAdmissionRejection();
                return false;
            }
        } while (Interlocked.CompareExchange(ref _inFlight, current + 1, current) != current);

        lease = new Lease(this);
        return true;
    }

    internal sealed class Lease(ZLinkAuthenticationAdmission owner) : IZLinkSerialTerminalRelease
    {
        private ZLinkAuthenticationAdmission? _owner = owner;

        public void Release()
        {
            if (Interlocked.Exchange(ref _owner, null) is { } admission)
                Interlocked.Decrement(ref admission._inFlight);
        }
    }
}

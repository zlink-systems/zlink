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

    internal bool IsAuthentication(ZlinkStreamHeader header) =>
        header.Kind == ZlinkStreamMessageKind.Request
        && string.Equals(header.Name, options.PacketName, StringComparison.Ordinal);

    internal Lease? TryAcquire(ulong queueEpoch)
    {
        int current;
        do
        {
            current = InFlight;
            if (current >= options.Limit)
            {
                ZLinkRuntimeMetrics.RecordAuthenticationAdmissionRejection();
                return null;
            }
        } while (Interlocked.CompareExchange(ref _inFlight, current + 1, current) != current);

        return new Lease(this, queueEpoch);
    }

    internal sealed class Lease(ZLinkAuthenticationAdmission owner, ulong queueEpoch)
        : IZLinkSerialTerminalRelease
    {
        private ZLinkAuthenticationAdmission? _owner = owner;
        internal ulong QueueEpoch { get; } = queueEpoch;

        public void Release()
        {
            if (Interlocked.Exchange(ref _owner, null) is { } admission)
                Interlocked.Decrement(ref admission._inFlight);
        }
    }
}

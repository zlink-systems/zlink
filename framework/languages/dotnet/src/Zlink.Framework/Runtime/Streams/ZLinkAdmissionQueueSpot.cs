using System.Buffers.Binary;
using System.Security.Cryptography;
using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.Runtime.Streams;

internal sealed class ZLinkAdmissionQueueSpot(
    IZLinkInstanceSpotContext context,
    ZLinkFrameworkRegistration registration
) : IZLinkInstanceSpot
{
    private ZLinkAdmissionQueue? _queue;
    public IZLinkInstanceSpotContext Context { get; } = context;

    public void Configure()
    {
        Context.Handlers.AddPacket<EnqueueHandler>();
        Context.Handlers.AddPacket<ReportHandler>();
        Context.Handlers.AddPacket<AdmitHandler>();
        Context.Handlers.AddPacket<ReleaseHandler>();
    }

    public ValueTask OnInitializeAsync(CancellationToken cancellationToken)
    {
        Span<byte> bytes = stackalloc byte[8];
        ulong epoch;
        do
        {
            RandomNumberGenerator.Fill(bytes);
            epoch = BinaryPrimitives.ReadUInt64BigEndian(bytes);
        } while (epoch == 0);
        _queue = new ZLinkAdmissionQueue(
            registration.AdmissionQueue!,
            registration.TimeProvider,
            epoch
        );
        return ValueTask.CompletedTask;
    }

    internal sealed class EnqueueHandler
        : IZLinkSpotRequestHandler<ZLinkAdmissionQueueSpot, AdmissionEnqueue, AdmissionEnqueued>
    {
        public ValueTask<AdmissionEnqueued> HandleAsync(
            ZLinkAdmissionQueueSpot spot,
            IZLinkMessageContext context,
            AdmissionEnqueue request,
            CancellationToken cancellationToken
        ) => ValueTask.FromResult(spot._queue!.Enqueue(request));
    }

    internal sealed class ReportHandler
        : IZLinkSpotRequestHandler<ZLinkAdmissionQueueSpot, AdmissionReport, AdmissionReported>
    {
        public ValueTask<AdmissionReported> HandleAsync(
            ZLinkAdmissionQueueSpot spot,
            IZLinkMessageContext context,
            AdmissionReport request,
            CancellationToken cancellationToken
        ) => spot._queue!.ReportAsync(request, spot.ReclaimExpiredOwnersAsync, cancellationToken);
    }

    internal sealed class AdmitHandler
        : IZLinkSpotRequestHandler<
            ZLinkAdmissionQueueSpot,
            AdmissionPermitRequest,
            AdmissionPermitResult
        >
    {
        public async ValueTask<AdmissionPermitResult> HandleAsync(
            ZLinkAdmissionQueueSpot spot,
            IZLinkMessageContext context,
            AdmissionPermitRequest request,
            CancellationToken cancellationToken
        )
        {
            var queue = spot._queue!;
            var permitOwners = queue.PermitOwnersAtLimit(request);
            if (permitOwners.Length != 0)
                await spot.ReclaimExpiredOwnersAsync(permitOwners, cancellationToken)
                    .ConfigureAwait(false);
            return queue.Admit(request);
        }
    }

    private async ValueTask ReclaimExpiredOwnersAsync(
        AdmissionOwnerLease[] permitOwners,
        CancellationToken cancellationToken
    )
    {
        var expired = await ZLinkSerialTurn
            .Current!.YieldFrameworkCallAsync(
                token => FindExpiredOwnersAsync(permitOwners, token),
                cancellationToken
            )
            .ConfigureAwait(false);
        foreach (var owner in expired)
            _queue!.ReclaimOwner(owner);
    }

    private async ValueTask<List<AdmissionOwnerLease>> FindExpiredOwnersAsync(
        AdmissionOwnerLease[] permitOwners,
        CancellationToken cancellationToken
    )
    {
        var store = registration.Locations.ResolveStore()!;
        var expired = new List<AdmissionOwnerLease>();
        foreach (var owner in permitOwners)
        {
            var lease = await store
                .ReadOwnerLeaseAsync(owner.OwnerId, cancellationToken)
                .ConfigureAwait(false);
            if (
                lease is not ZLinkOwnerLeaseReadResult.Found found
                || found.LeaseExpiresAt <= found.StoreNow
                || (ulong)found.Token.LeaseGeneration != owner.Generation
            )
                expired.Add(owner);
        }
        return expired;
    }

    internal sealed class ReleaseHandler
        : IZLinkSpotPacketHandler<ZLinkAdmissionQueueSpot, AdmissionPermitRelease>
    {
        public ValueTask HandleAsync(
            ZLinkAdmissionQueueSpot spot,
            IZLinkMessageContext context,
            AdmissionPermitRelease request,
            CancellationToken cancellationToken
        )
        {
            spot._queue!.Release(request);
            return ValueTask.CompletedTask;
        }
    }
}

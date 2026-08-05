using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;
using SpotActorTransfer.Shared;
using Systems.Zlink;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Contracts.Streams;
namespace SpotActorTransfer.ActorNode
{
    internal sealed class TransferActor(
        string actorId,
        IZLinkActorContext context,
        EvidenceStore evidence) : IZLinkActor
    {
        private readonly Queue<JoinTargetReq> _pendingJoins = new();

        public string ActorId { get; } = actorId;

        public string ActorType { get; set; } = SpotActorTransferNames.ActorTypeStateful;

        public int StateVersion { get; set; }

        public byte[] ApplicationState { get; set; } = [];

        public IZLinkActorContext Context { get; } = context;

        public void RecordDeferredJoin(JoinTargetReq request) =>
            _pendingJoins.Enqueue(request);

        public ValueTask OnJoinCompletedAsync(
            ZLinkActorJoinCompletion completion,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var reply = completion switch
            {
                ZLinkActorJoinCompletion.Accepted { Reply: { } replyMessage } =>
                    replyMessage.Decode<JoinTargetRes>(),
                ZLinkActorJoinCompletion.Rejected { Reply: { } replyMessage } =>
                    replyMessage.Decode<JoinTargetRes>(),
                _ => null
            };
            var pending = _pendingJoins.Count > 0 ? _pendingJoins.Dequeue() : null;
            var scenario = reply?.Scenario ?? pending?.Scenario ?? "unknown";
            var targetSpotId = reply?.TargetSpotId ?? pending?.TargetSpotId ?? string.Empty;
            var kind = completion switch
            {
                ZLinkActorJoinCompletion.Accepted => "success_reply",
                ZLinkActorJoinCompletion.Rejected => "reject_reply",
                ZLinkActorJoinCompletion.Failed => "join_failed",
                _ => throw new InvalidOperationException("Unknown Actor Join completion.")
            };
            var terminalValue = completion is ZLinkActorJoinCompletion.Failed failed
                ? failed.Kind.ToString()
                : targetSpotId;
            evidence.Add(scenario, ActorId, kind, terminalValue);
            if (scenario == "ST-A2"
                && completion is ZLinkActorJoinCompletion.Rejected)
                evidence.Add(
                    scenario,
                    ActorId,
                    "typed_reject_reply",
                    $"accepted={reply?.Accepted};spot={targetSpotId}");
            return ValueTask.CompletedTask;
        }
    }

    internal sealed class TransferActorFactory(
        EvidenceStore evidence) : IZLinkActorFactory<TransferActor>
    {
        public ValueTask<TransferActor> CreateAsync(
            IZLinkActorContext context,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (evidence.NodeRid == "actor-b"
                && context.ActorId.StartsWith("actor-no-adapter-", StringComparison.Ordinal))
                evidence.Add("transfer", context.ActorId, "transfer_in_empty_default", "actor-factory");
            return ValueTask.FromResult(new TransferActor(
                context.ActorId,
                context,
                evidence));
        }
    }

    internal sealed class TransferActorRelocationAdapter(
        EvidenceStore evidence,
        TransferGateStore transferGates)
        : IZLinkActorRelocationAdapter<TransferActor>
    {
        public async ValueTask<byte[]> CaptureAsync(
            TransferActor actor,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (actor.ActorId.Contains(
                    "slow-capture",
                    StringComparison.Ordinal))
            {
                evidence.Add(
                    actor.ActorId.Contains(
                        "mf-ar-hold",
                        StringComparison.Ordinal)
                        ? "MF-AR-HOLD-SLOW-CAPTURE"
                        : "ST-G5",
                    actor.ActorId,
                    "slow_capture_started",
                    "1250ms");
                await Task.Delay(1_250, cancellationToken)
                    .ConfigureAwait(false);
            }
            if (actor.ActorType == SpotActorTransferNames.ActorTypeFailTransferOut)
            {
                evidence.Add("ST-C3", actor.ActorId, "transfer_out_failed", actor.StateVersion.ToString());
                throw new InvalidOperationException("injected transfer out failure");
            }

            if (actor.ActorType == SpotActorTransferNames.ActorTypeEmptyState)
            {
                evidence.Add("transfer", actor.ActorId, "transfer_out_empty", "custom-adapter");
                return [];
            }

            evidence.Add(
                "transfer",
                actor.ActorId,
                "application_capture_started",
                "actor");
            var payload = TransferActorStateCodec.Encode(actor);
            evidence.Add(
                "transfer",
                actor.ActorId,
                "transfer_out",
                actor.StateVersion.ToString());
            evidence.Add(
                "transfer",
                actor.ActorId,
                "application_payload",
                $"bytes={payload.Length};sha256={TransferActorStateCodec.Sha256(payload)}");
            if (actor.ActorId.StartsWith("actor-source-down-before-commit-", StringComparison.Ordinal))
            {
                evidence.Add("ST-C1", actor.ActorId, "before_commit_gate", actor.StateVersion.ToString());
                await transferGates.WaitAsync(actor.ActorId, cancellationToken)
                    .ConfigureAwait(false);
            }

            return payload;
        }

        public async ValueTask RestoreAsync(
            TransferActor actor,
            ReadOnlyMemory<byte> payload,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var actorId = actor.Context.ActorId;
            evidence.Add(
                "transfer",
                actorId,
                "application_restore_started",
                "actor");
            evidence.Add(
                "transfer",
                actorId,
                "relocation_payload_restored",
                $"bytes={payload.Length};sha256="
                + TransferActorStateCodec.Sha256(payload.Span));
            if (actorId.Contains(
                    "slow-restore",
                    StringComparison.Ordinal))
            {
                evidence.Add(
                    actorId.Contains(
                        "mf-ar-hold",
                        StringComparison.Ordinal)
                        ? "MF-AR-HOLD-SLOW-RESTORE"
                        : "ST-G5",
                    actorId,
                    "slow_restore_started",
                    "1250ms");
                await Task.Delay(1_250, cancellationToken)
                    .ConfigureAwait(false);
            }
            if (payload.IsEmpty)
            {
                evidence.Add("transfer", actorId, "transfer_in_empty", "custom-adapter");
                actor.ActorType =
                    SpotActorTransferNames.ActorTypeEmptyState;
                return;
            }

            var state = TransferActorStateCodec.Decode(actorId, payload.Span);
            if (actorId.StartsWith("actor-fail-transfer-in-", StringComparison.Ordinal))
            {
                evidence.Add(
                    "ST-C3",
                    actorId,
                    "transfer_in_failed",
                    state.StateVersion.ToString());
                throw new InvalidOperationException("injected transfer in failure");
            }

            actor.ActorType = SpotActorTransferNames.ActorTypeStateful;
            actor.StateVersion = state.StateVersion;
            actor.ApplicationState = state.ApplicationState;
            evidence.Add("transfer", actorId, "transfer_in", actor.StateVersion.ToString());
            evidence.Add(
                "transfer",
                actorId,
                "application_state_restored",
                $"bytes={actor.ApplicationState.Length};sha256="
                + TransferActorStateCodec.Sha256(actor.ApplicationState));
        }
    }

    internal sealed class TransferEntrySpot(
        IZLinkEntrySpotContext context,
        EvidenceStore evidence,
        DomainStateStore domainState,
        TransferGateStore transferGates)
        : IZLinkEntrySpot<TransferActor>
    {
        public IZLinkEntrySpotContext Context { get; } = context;

        public ValueTask<ZLinkActorCreateResponse> OnCreateActorAsync(
            TransferActor actor,
            ZLinkMessage createRequest,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (!createRequest.IsEmpty)
            {
                var request = createRequest.Decode<ActorCreateReq>();
                actor.ActorType = request.ActorType;
                actor.StateVersion = request.StateVersion;
                actor.ApplicationState = TransferActorStateCodec.CreateState(
                    actor.ActorId,
                    request.ApplicationStateBytes);
                if (actor.ActorType == SpotActorTransferNames.ActorTypeEmptyState)
                    domainState.Save(actor.ActorId, actor.StateVersion);
            }

            evidence.Add("create", actor.ActorId, "create", $"{actor.ActorType}:{actor.StateVersion}");
            return ValueTask.FromResult(ZLinkActorCreateResponse.Accept());
        }

        public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
            string actorId,
            ZLinkMessage request,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            evidence.Add("local", actorId, "admission", "actor-id-only");
            return ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept(request));
        }

        public ValueTask OnJoinedActorAsync(
            TransferActor actor,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            evidence.Add("local", actor.ActorId, "entry_joined", actor.StateVersion.ToString());
            return ValueTask.CompletedTask;
        }

        public async ValueTask OnLeaveActorAsync(
            TransferActor actor,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (actor.ActorId.StartsWith(
                    "actor-cleanup-after-success-",
                    StringComparison.Ordinal))
            {
                evidence.Add(
                    "ST-B2",
                    actor.ActorId,
                    "source_cleanup_wait",
                    "entry-spot-leave");
                await transferGates.WaitAsync(
                        actor.ActorId,
                        cancellationToken)
                    .ConfigureAwait(false);
            }
            if (actor.ActorId.StartsWith(
                    "actor-st-g5-slow-cleanup-",
                    StringComparison.Ordinal))
            {
                evidence.Add(
                    "ST-G5",
                    actor.ActorId,
                    "slow_source_cleanup_started",
                    "1250ms");
                await Task.Delay(1_250, cancellationToken)
                    .ConfigureAwait(false);
            }
            if (actor.ActorType == SpotActorTransferNames.ActorTypeNoAdapter)
                evidence.Add("transfer", actor.ActorId, "transfer_out_empty_default", "no-adapter");
            if (actor.ActorType == SpotActorTransferNames.ActorTypeFailLeave)
            {
                evidence.Add("ST-C3", actor.ActorId, "leave_failed", actor.StateVersion.ToString());
                throw new InvalidOperationException("injected source leave failure");
            }

            evidence.Add("transfer", actor.ActorId, "leave", actor.StateVersion.ToString());
        }
    }

    internal static class TransferActorStateCodec
    {
        private const uint Magic = 0x5a4c5331;

        internal static byte[] CreateState(string actorId, int size)
        {
            if (size < 0)
                throw new ArgumentOutOfRangeException(nameof(size));
            if (size == 0)
                return [];

            var result = new byte[size];
            var seed = SHA256.HashData(Encoding.UTF8.GetBytes(actorId));
            ulong state = BinaryPrimitives.ReadUInt64LittleEndian(seed);
            for (var index = 0; index < result.Length; index++)
            {
                state ^= state << 13;
                state ^= state >> 7;
                state ^= state << 17;
                result[index] = (byte)state;
            }
            return result;
        }

        internal static byte[] Encode(TransferActor actor)
        {
            var actorId = Encoding.UTF8.GetBytes(actor.ActorId);
            var payload = new byte[
                sizeof(uint)
                + sizeof(int)
                + sizeof(int)
                + actorId.Length
                + actor.ApplicationState.Length];
            var span = payload.AsSpan();
            BinaryPrimitives.WriteUInt32LittleEndian(span, Magic);
            BinaryPrimitives.WriteInt32LittleEndian(
                span[sizeof(uint)..],
                actor.StateVersion);
            BinaryPrimitives.WriteInt32LittleEndian(
                span[(sizeof(uint) + sizeof(int))..],
                actorId.Length);
            actorId.CopyTo(span[(sizeof(uint) + sizeof(int) + sizeof(int))..]);
            actor.ApplicationState.CopyTo(
                span[(sizeof(uint) + sizeof(int) + sizeof(int) + actorId.Length)..]);
            return payload;
        }

        internal static (
            int StateVersion,
            byte[] ApplicationState) Decode(
            string expectedActorId,
            ReadOnlySpan<byte> payload)
        {
            const int headerSize = sizeof(uint) + sizeof(int) + sizeof(int);
            if (payload.Length < headerSize
                || BinaryPrimitives.ReadUInt32LittleEndian(payload) != Magic)
            {
                throw new InvalidDataException(
                    "Actor relocation state header is invalid.");
            }

            var stateVersion = BinaryPrimitives.ReadInt32LittleEndian(
                payload[sizeof(uint)..]);
            var actorIdLength = BinaryPrimitives.ReadInt32LittleEndian(
                payload[(sizeof(uint) + sizeof(int))..]);
            if (actorIdLength < 0
                || headerSize + actorIdLength > payload.Length)
            {
                throw new InvalidDataException(
                    "Actor relocation identity length is invalid.");
            }

            var actorId = Encoding.UTF8.GetString(
                payload.Slice(headerSize, actorIdLength));
            if (!string.Equals(
                    actorId,
                    expectedActorId,
                    StringComparison.Ordinal))
            {
                throw new InvalidDataException(
                    "Actor relocation identity does not match the target Actor.");
            }

            return (
                stateVersion,
                payload[(headerSize + actorIdLength)..].ToArray());
        }

        internal static string Sha256(ReadOnlySpan<byte> payload) =>
            Convert.ToHexString(SHA256.HashData(payload)).ToLowerInvariant();
    }

    internal sealed class TransferUserSpot(
        IZLinkSpotContext context,
        EvidenceStore evidence,
        JoinedGateStore joinedGates,
        DomainStateStore domainState) : IZLinkSpot<TransferActor>
    {
        private string _mode = "accept";
        private readonly Dictionary<string, string> _joinScenarios = new(StringComparer.Ordinal);

        public IZLinkSpotContext Context { get; } = context;

        public void Configure()
        {
            Context.Handlers.AddActorPacket<UserSpotJoinTargetHandler, TransferActor>(
                nameof(JoinTargetReq));
            Context.Handlers.AddActorPacket<ProbeHandler, TransferActor>(
                nameof(ProbeReq));
            Context.Handlers.AddActorPacket<HandoffPacketHandler, TransferActor>(
                nameof(HandoffPacket));
            Context.Handlers.AddActorPacket<BoundPushHandler, TransferActor>(
                nameof(BoundPushReq));
        }

        public ValueTask<ZLinkSpotCreateResponse> OnCreateAsync(
            ZLinkMessage request,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (!request.IsEmpty) _mode = request.Decode<CreateSpotReq>().Mode;
            evidence.Add("create_spot", Context.SpotId, "spot_created", _mode);
            return ValueTask.FromResult(ZLinkSpotCreateResponse.Accept());
        }

        public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
            string actorId,
            ZLinkMessage request,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var join = request.Decode<JoinTargetReq>();
            _joinScenarios[actorId] = join.Scenario;
            evidence.Add(join.Scenario, actorId, "admission", $"spot={Context.SpotId}|mode={_mode}|input=actor-id-only");
            if (string.Equals(_mode, "reject", StringComparison.Ordinal)
                || string.Equals(join.ExpectedMode, "reject", StringComparison.Ordinal))
                return ValueTask.FromResult(ZLinkSpotActorJoinResult.Reject(new JoinTargetRes(
                    join.Scenario,
                    actorId,
                    false,
                    string.Empty,
                    Context.SpotId,
                    0)));
            return ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept(new JoinTargetRes(
                join.Scenario,
                actorId,
                true,
                string.Empty,
                Context.SpotId,
                0)));
        }

        public ValueTask OnJoinedActorAsync(
            TransferActor actor,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (string.Equals(_mode, "delay-joined", StringComparison.Ordinal))
            {
                var scenario = _joinScenarios.GetValueOrDefault(actor.ActorId, "unknown");
                evidence.Add(scenario, actor.ActorId, "joined_wait", Context.SpotId);
                return WaitForJoinedGateAsync(actor, cancellationToken);
            }
            if (string.Equals(_mode, "fail-joined", StringComparison.Ordinal))
            {
                var scenario = _joinScenarios.GetValueOrDefault(actor.ActorId, "unknown");
                evidence.Add(scenario, actor.ActorId, "joined_failed", Context.SpotId);
                throw new InvalidOperationException("injected joined failure");
            }

            evidence.Add("transfer", actor.ActorId, "joined", $"{Context.SpotId}:{actor.StateVersion}");
            if (actor.ActorType == SpotActorTransferNames.ActorTypeEmptyState)
            {
                actor.StateVersion = domainState.Load(actor.ActorId);
                evidence.Add("transfer", actor.ActorId, "domain_state_loaded", actor.ActorId);
            }
            return ValueTask.CompletedTask;
        }

        private async ValueTask WaitForJoinedGateAsync(
            TransferActor actor,
            CancellationToken cancellationToken)
        {
            var scenario = _joinScenarios.GetValueOrDefault(actor.ActorId, "unknown");
            await joinedGates.WaitAsync(Context.SpotId, cancellationToken)
                .ConfigureAwait(false);
            evidence.Add(scenario, actor.ActorId, "joined_released", Context.SpotId);
            evidence.Add("transfer", actor.ActorId, "joined", $"{Context.SpotId}:{actor.StateVersion}");
        }

        public ValueTask OnLeaveActorAsync(
            TransferActor actor,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            evidence.Add("transfer", actor.ActorId, "target_leave", Context.SpotId);
            return ValueTask.CompletedTask;
        }
    }

    internal sealed class ActorJoinTargetUseCase(
        EvidenceStore evidence,
        TransferGateStore transferGates)
    {
        public async ValueTask<JoinTargetRes> ExecuteAsync(
            TransferActor actor,
            JoinTargetReq request,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            actor.RecordDeferredJoin(request);
            if (request.Scenario == "ST-H1")
            {
                var mutableRequest = new MutableJoinTargetReq
                {
                    Scenario = request.Scenario,
                    TargetSpotId = request.TargetSpotId,
                    ExpectedMode = request.ExpectedMode
                };
                actor.Context.JoinSpot(request.TargetSpotId, mutableRequest)
                    .Timeout(TimeSpan.FromSeconds(10))
                    .Defer();
                evidence.Add(
                    request.Scenario,
                    actor.ActorId,
                    "defer_registered",
                    request.TargetSpotId);

                // Defer must snapshot the request synchronously. Mutating the
                // application object after registration cannot change target
                // admission or completion.
                mutableRequest.Scenario = "ST-H1-MUTATED";
                mutableRequest.TargetSpotId = "mutated-target";
                mutableRequest.ExpectedMode = "reject";
                await Task.Delay(
                        TimeSpan.FromMilliseconds(300),
                        cancellationToken)
                    .ConfigureAwait(false);
                evidence.Add(
                    request.Scenario,
                    actor.ActorId,
                    "defer_handler_completed",
                    request.TargetSpotId);
            }
            else
            {
                actor.Context.JoinSpot(request.TargetSpotId, request)
                    .Timeout(TimeSpan.FromSeconds(10))
                    .Defer();
                if (request.Scenario == "ST-B1")
                {
                    evidence.Add(
                        request.Scenario,
                        actor.ActorId,
                        "defer_registered",
                        request.TargetSpotId);
                    await transferGates.WaitAsync(
                            actor.ActorId,
                            cancellationToken)
                        .ConfigureAwait(false);
                    evidence.Add(
                        request.Scenario,
                        actor.ActorId,
                        "defer_handler_released",
                        request.TargetSpotId);
                }
            }
            evidence.Add(request.Scenario, actor.ActorId, "commit_request", request.TargetSpotId);
            return new JoinTargetRes(
                request.Scenario,
                actor.ActorId,
                true,
                evidence.NodeRid,
                request.TargetSpotId,
                actor.StateVersion);
        }
    }

    internal sealed class JoinTargetHandler(ActorJoinTargetUseCase joinTarget)
        : IZLinkEntrySpotActorRequestHandler<TransferEntrySpot, TransferActor, JoinTargetReq, JoinTargetRes>
    {
        public async ValueTask<JoinTargetRes> HandleAsync(
            TransferEntrySpot entrySpot,
            TransferActor actor,
            IZLinkMessageContext context,
            JoinTargetReq request,
            CancellationToken cancellationToken)
        {
            _ = entrySpot;
            _ = context;
            return await joinTarget.ExecuteAsync(actor, request, cancellationToken)
                .ConfigureAwait(false);
        }
    }

    [ZLinkSpotActorRequestHandler(nameof(JoinTargetReq))]
    internal sealed class UserSpotJoinTargetHandler(ActorJoinTargetUseCase joinTarget)
        : IZLinkSpotActorRequestHandler<TransferUserSpot, TransferActor, JoinTargetReq, JoinTargetRes>
    {
        public async ValueTask<JoinTargetRes> HandleAsync(
            TransferUserSpot spot,
            TransferActor actor,
            IZLinkMessageContext context,
            JoinTargetReq request,
            CancellationToken cancellationToken)
        {
            _ = spot;
            _ = context;
            return await joinTarget.ExecuteAsync(actor, request, cancellationToken)
                .ConfigureAwait(false);
        }
    }

    [ZLinkSpotActorRequestHandler(nameof(ProbeReq))]
    internal sealed class ProbeHandler(EvidenceStore evidence)
        : IZLinkSpotActorRequestHandler<TransferUserSpot, TransferActor, ProbeReq, ProbeRes>
    {
        public async ValueTask<ProbeRes> HandleAsync(
            TransferUserSpot spot,
            TransferActor actor,
            IZLinkMessageContext context,
            ProbeReq request,
            CancellationToken cancellationToken)
        {
            _ = context;
            cancellationToken.ThrowIfCancellationRequested();
            evidence.Add(request.Scenario, actor.ActorId, "packet_handler", request.Marker);
            if ((request.Scenario == "ST-F6"
                    && request.Marker == "late-reply")
                || (request.Scenario == "ST-I5"
                    && request.Marker.StartsWith(
                        "deadline-",
                        StringComparison.Ordinal)))
            {
                var delay = request.Scenario == "ST-I5"
                    ? TimeSpan.FromSeconds(7)
                    : TimeSpan.FromSeconds(1);
                await Task.Delay(delay, cancellationToken);
                evidence.Add(request.Scenario, actor.ActorId, "late_reply_created", request.Marker);
            }
            return new ProbeRes(
                request.Scenario,
                actor.ActorId,
                spot.Context.SpotId,
                spot.Context.NodeRid.ToString(),
                actor.StateVersion,
                request.ReplyMarker ?? request.Marker);
        }
    }

    [ZLinkSpotActorSendHandler(nameof(HandoffPacket))]
    internal sealed class HandoffPacketHandler(EvidenceStore evidence)
        : IZLinkSpotActorSendHandler<TransferUserSpot, TransferActor, HandoffPacket>
    {
        public ValueTask HandleAsync(
            TransferUserSpot spot,
            TransferActor actor,
            IZLinkMessageContext context,
            HandoffPacket message,
            CancellationToken cancellationToken)
        {
            _ = spot;
            _ = context;
            cancellationToken.ThrowIfCancellationRequested();
            evidence.Add(message.Scenario, actor.ActorId, "handoff_packet", message.Marker);
            if (message.Scenario == "ST-A2")
                evidence.Add(
                    message.Scenario,
                    actor.ActorId,
                    "user_handoff_packet",
                    $"{message.Marker}:state={actor.StateVersion}");
            return ValueTask.CompletedTask;
        }
    }

    [ZLinkSpotActorSendHandler(nameof(HandoffPacket))]
    internal sealed class EntryHandoffPacketHandler(EvidenceStore evidence)
        : IZLinkEntrySpotActorSendHandler<TransferEntrySpot, TransferActor, HandoffPacket>
    {
        public ValueTask HandleAsync(
            TransferEntrySpot entrySpot,
            TransferActor actor,
            IZLinkMessageContext context,
            HandoffPacket message,
            CancellationToken cancellationToken)
        {
            _ = entrySpot;
            _ = context;
            cancellationToken.ThrowIfCancellationRequested();
            evidence.Add(message.Scenario, actor.ActorId, "handoff_packet", message.Marker);
            if (message.Scenario == "ST-A2")
                evidence.Add(
                    message.Scenario,
                    actor.ActorId,
                    "entry_handoff_packet",
                    $"{message.Marker}:state={actor.StateVersion}");
            return ValueTask.CompletedTask;
        }
    }

    [ZLinkSpotActorRequestHandler(nameof(BoundPushReq))]
    internal sealed class EntryBoundPushHandler(EvidenceStore evidence)
        : IZLinkEntrySpotActorRequestHandler<TransferEntrySpot, TransferActor, BoundPushReq, BoundPushRes>
    {
        public async ValueTask<BoundPushRes> HandleAsync(
            TransferEntrySpot entrySpot,
            TransferActor actor,
            IZLinkMessageContext context,
            BoundPushReq request,
            CancellationToken cancellationToken)
        {
            _ = context;
            cancellationToken.ThrowIfCancellationRequested();
            await actor.Context.BoundSession.Send(new BoundPushNotify(
                    request.Scenario,
                    actor.ActorId,
                    entrySpot.Context.SpotId,
                    entrySpot.Context.NodeRid.ToString(),
                    request.Marker,
                    actor.StateVersion))
                .Async(cancellationToken);
            evidence.Add(request.Scenario, actor.ActorId, "bound_push", request.Marker);
            return new BoundPushRes(
                request.Scenario,
                actor.ActorId,
                entrySpot.Context.SpotId,
                entrySpot.Context.NodeRid.ToString(),
                request.Marker,
                actor.StateVersion);
        }
    }

    [ZLinkSpotActorRequestHandler(nameof(BoundPushReq))]
    internal sealed class BoundPushHandler(EvidenceStore evidence)
        : IZLinkSpotActorRequestHandler<TransferUserSpot, TransferActor, BoundPushReq, BoundPushRes>
    {
        public async ValueTask<BoundPushRes> HandleAsync(
            TransferUserSpot spot,
            TransferActor actor,
            IZLinkMessageContext context,
            BoundPushReq request,
            CancellationToken cancellationToken)
        {
            _ = context;
            cancellationToken.ThrowIfCancellationRequested();
            await actor.Context.BoundSession.Send(new BoundPushNotify(
                    request.Scenario,
                    actor.ActorId,
                    spot.Context.SpotId,
                    spot.Context.NodeRid.ToString(),
                    request.Marker,
                    actor.StateVersion))
                .Async(cancellationToken);
            evidence.Add(request.Scenario, actor.ActorId, "bound_push", request.Marker);
            return new BoundPushRes(
                request.Scenario,
                actor.ActorId,
                spot.Context.SpotId,
                spot.Context.NodeRid.ToString(),
                request.Marker,
                actor.StateVersion);
        }
    }

    internal sealed class RelocationPayloadUserSpot(
        IZLinkSpotContext context) : IZLinkSpot<TransferActor>
    {
        public IZLinkSpotContext Context { get; } = context;

        internal string Scenario { get; private set; } = "ST-I1";
        internal byte[] ApplicationState { get; set; } = [];

        public ValueTask<ZLinkSpotCreateResponse> OnCreateAsync(
            ZLinkMessage request,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var profile = request.Decode<RelocationPayloadSpotReq>();
            Scenario = profile.Scenario;
            ApplicationState = TransferActorStateCodec.CreateState(
                Context.SpotId,
                profile.ApplicationStateBytes);
            return ValueTask.FromResult(ZLinkSpotCreateResponse.Accept());
        }

        public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
            string actorId,
            ZLinkMessage request,
            CancellationToken cancellationToken)
        {
            _ = actorId;
            cancellationToken.ThrowIfCancellationRequested();
            return ValueTask.FromResult(
                ZLinkSpotActorJoinResult.Accept(request));
        }

        public ValueTask OnJoinedActorAsync(
            TransferActor actor,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            return ValueTask.CompletedTask;
        }

        public ValueTask OnLeaveActorAsync(
            TransferActor actor,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            return ValueTask.CompletedTask;
        }
    }

    internal sealed class RelocationPayloadInstanceSpot(
        IZLinkInstanceSpotContext context) : IZLinkInstanceSpot
    {
        public IZLinkInstanceSpotContext Context { get; } = context;

        internal string Scenario { get; set; } = "ST-I1";
        internal byte[] ApplicationState { get; set; } = [];

    }

    internal sealed class RelocationReadyUserSpot(
        IZLinkSpotContext context,
        EvidenceStore evidence) : IZLinkSpot
    {
        public IZLinkSpotContext Context { get; } = context;

        internal string Scenario { get; set; } = "ST-G6";

        public ValueTask<ZLinkSpotCreateResponse> OnCreateAsync(
            ZLinkMessage request,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            Scenario = request.Decode<RelocationPayloadSpotReq>().Scenario;
            return ValueTask.FromResult(ZLinkSpotCreateResponse.Accept());
        }

        public ValueTask OnRelocationReadyCompletedAsync(
            ZLinkSpotRelocationReadyCompletion completion,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            evidence.Add(
                Scenario,
                Context.SpotId,
                "relocation_ready_completed",
                $"{completion.Outcome}:{Context.NodeRid}");
            return ValueTask.CompletedTask;
        }
    }

    internal sealed class RelocationReadyDefaultUserSpot(
        IZLinkSpotContext context) : IZLinkSpot
    {
        public IZLinkSpotContext Context { get; } = context;
    }

    internal sealed class RelocationReadyUserSpotAdapter
        : IZLinkSpotRelocationAdapter<RelocationReadyUserSpot>
    {
        public ValueTask<byte[]> CaptureAsync(
            RelocationReadyUserSpot spot,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (spot.Scenario.Contains(
                    "PRECOMMIT-ABORT",
                    StringComparison.Ordinal))
            {
                throw new InvalidOperationException(
                    "Injected ST-G6 precommit abort.");
            }
            return ValueTask.FromResult(
                Encoding.UTF8.GetBytes(spot.Scenario));
        }

        public ValueTask RestoreAsync(
            RelocationReadyUserSpot spot,
            ReadOnlyMemory<byte> payload,
            CancellationToken cancellationToken)
        {
            _ = spot;
            cancellationToken.ThrowIfCancellationRequested();
            spot.Scenario = Encoding.UTF8.GetString(payload.Span);
            return ValueTask.CompletedTask;
        }
    }

    internal static class RelocationReadySignal
    {
        internal static async ValueTask<RelocationReadySignalRes> ExecuteAsync(
            IZLinkSpotContext context,
            RelocationReadySignalReq request,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            context.RelocationReady().Defer();
            var secondRejected = !request.DeferTwice;
            if (request.DeferTwice)
            {
                try
                {
                    context.RelocationReady().Defer();
                }
                catch (InvalidOperationException)
                {
                    secondRejected = true;
                }
            }

            var operationRejected =
                !request.StartFrameworkOperationAfterDefer;
            if (request.StartFrameworkOperationAfterDefer)
            {
                try
                {
                    _ = await context.CloseAsync(cancellationToken)
                        .ConfigureAwait(false);
                }
                catch (InvalidOperationException)
                {
                    operationRejected = true;
                }
            }

            return new RelocationReadySignalRes(
                context.SpotId,
                context.NodeRid.ToString(),
                Deferred: true,
                SecondDeferRejected: secondRejected,
                FrameworkOperationRejected: operationRejected);
        }
    }

    internal sealed class RelocationReadyUserSpotSignalHandler
        : IZLinkSpotRequestHandler<
            RelocationReadyUserSpot,
            RelocationReadySignalReq,
            RelocationReadySignalRes>
    {
        public ValueTask<RelocationReadySignalRes> HandleAsync(
            RelocationReadyUserSpot spot,
            RelocationReadySignalReq request,
            CancellationToken cancellationToken) =>
            RelocationReadySignal.ExecuteAsync(
                spot.Context,
                request,
                cancellationToken);
    }

    internal sealed class RelocationReadyDefaultUserSpotSignalHandler
        : IZLinkSpotRequestHandler<
            RelocationReadyDefaultUserSpot,
            RelocationReadySignalReq,
            RelocationReadySignalRes>
    {
        public ValueTask<RelocationReadySignalRes> HandleAsync(
            RelocationReadyDefaultUserSpot spot,
            RelocationReadySignalReq request,
            CancellationToken cancellationToken) =>
            RelocationReadySignal.ExecuteAsync(
                spot.Context,
                request,
                cancellationToken);
    }

    internal sealed class AnyTurnRelocationReadyNegativeHandler
        : IZLinkSpotRequestHandler<
            RelocationPayloadUserSpot,
            RelocationReadySignalReq,
            RelocationReadySignalRes>
    {
        public ValueTask<RelocationReadySignalRes> HandleAsync(
            RelocationPayloadUserSpot spot,
            RelocationReadySignalReq request,
            CancellationToken cancellationToken) =>
            RelocationReadySignal.ExecuteAsync(
                spot.Context,
                request,
                cancellationToken);
    }

    internal sealed class PerActorRelocationReadyNegativeHandler
        : IZLinkSpotRequestHandler<
            RelocationPayloadPerActorUserSpot,
            RelocationReadySignalReq,
            RelocationReadySignalRes>
    {
        public ValueTask<RelocationReadySignalRes> HandleAsync(
            RelocationPayloadPerActorUserSpot spot,
            RelocationReadySignalReq request,
            CancellationToken cancellationToken) =>
            RelocationReadySignal.ExecuteAsync(
                spot.Context,
                request,
                cancellationToken);
    }

    internal sealed class RelocationPayloadPerActorUserSpot(
        IZLinkSpotContext context) : IZLinkSpot<TransferActor>
    {
        public IZLinkSpotContext Context { get; } = context;

        internal string Scenario { get; private set; } = "ST-G3";
        internal byte[] ApplicationState { get; set; } = [];

        public ValueTask<ZLinkSpotCreateResponse> OnCreateAsync(
            ZLinkMessage request,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var profile = request.Decode<RelocationPayloadSpotReq>();
            Scenario = profile.Scenario;
            ApplicationState = TransferActorStateCodec.CreateState(
                Context.SpotId,
                profile.ApplicationStateBytes);
            return ValueTask.FromResult(ZLinkSpotCreateResponse.Accept());
        }

        public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
            string actorId,
            ZLinkMessage request,
            CancellationToken cancellationToken)
        {
            _ = actorId;
            cancellationToken.ThrowIfCancellationRequested();
            return ValueTask.FromResult(
                ZLinkSpotActorJoinResult.Accept(request));
        }

        public ValueTask OnJoinedActorAsync(
            TransferActor actor,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            return ValueTask.CompletedTask;
        }

        public ValueTask OnLeaveActorAsync(
            TransferActor actor,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            return ValueTask.CompletedTask;
        }
    }

    internal sealed class RelocationPayloadInstanceSpotHandler
        : IZLinkSpotRequestHandler<
            RelocationPayloadInstanceSpot,
            RelocationPayloadSpotReq,
            RelocationPayloadSpotRes>
    {
        public ValueTask<RelocationPayloadSpotRes> HandleAsync(
            RelocationPayloadInstanceSpot spot,
            RelocationPayloadSpotReq request,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (spot.ApplicationState.Length == 0)
            {
                spot.Scenario = request.Scenario;
                spot.ApplicationState = TransferActorStateCodec.CreateState(
                    spot.Context.SpotId,
                    request.ApplicationStateBytes);
            }
            return ValueTask.FromResult(new RelocationPayloadSpotRes(
                spot.Context.SpotId,
                spot.Context.NodeRid.ToString(),
                checked((long)spot.Context.ObjectGeneration),
                spot.ApplicationState.Length,
                TransferActorStateCodec.Sha256(spot.ApplicationState)));
        }
    }

    internal sealed class RelocationPayloadUserSpotAdapter(
        EvidenceStore evidence)
        : IZLinkSpotRelocationAdapter<RelocationPayloadUserSpot>
    {
        public async ValueTask<byte[]> CaptureAsync(
            RelocationPayloadUserSpot spot,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (spot.Scenario.Contains(
                    "SLOW-CAPTURE",
                    StringComparison.Ordinal))
            {
                evidence.Add(
                    spot.Scenario,
                    spot.Context.SpotId,
                    "slow_capture_started",
                    "1250ms");
                await Task.Delay(1_250, cancellationToken)
                    .ConfigureAwait(false);
            }
            evidence.Add(
                spot.Scenario,
                spot.Context.SpotId,
                "spot_application_capture_started",
                "spotwide");
            evidence.Add(
                spot.Scenario,
                spot.Context.SpotId,
                "spot_application_payload",
                $"kind=spotwide;bytes={spot.ApplicationState.Length};sha256="
                + TransferActorStateCodec.Sha256(spot.ApplicationState));
            return spot.ApplicationState;
        }

        public ValueTask RestoreAsync(
            RelocationPayloadUserSpot spot,
            ReadOnlyMemory<byte> payload,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            evidence.Add(
                "ST-I1",
                spot.Context.SpotId,
                "spot_application_restore_started",
                "spotwide");
            spot.ApplicationState = payload.ToArray();
            evidence.Add(
                "ST-I1",
                spot.Context.SpotId,
                "spot_application_state_restored",
                $"kind=spotwide;bytes={payload.Length};sha256="
                + TransferActorStateCodec.Sha256(payload.Span));
            return ValueTask.CompletedTask;
        }
    }

    internal sealed class RelocationPayloadInstanceSpotAdapter(
        EvidenceStore evidence)
        : IZLinkSpotRelocationAdapter<RelocationPayloadInstanceSpot>
    {
        public async ValueTask<byte[]> CaptureAsync(
            RelocationPayloadInstanceSpot spot,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (spot.Scenario.Contains(
                    "SLOW-CAPTURE",
                    StringComparison.Ordinal))
            {
                evidence.Add(
                    spot.Scenario,
                    spot.Context.SpotId,
                    "slow_capture_started",
                    "1250ms");
                await Task.Delay(1_250, cancellationToken)
                    .ConfigureAwait(false);
            }
            evidence.Add(
                spot.Scenario,
                spot.Context.SpotId,
                "spot_application_payload",
                $"kind=instance;bytes={spot.ApplicationState.Length};sha256="
                + TransferActorStateCodec.Sha256(spot.ApplicationState));
            return spot.ApplicationState;
        }

        public async ValueTask RestoreAsync(
            RelocationPayloadInstanceSpot spot,
            ReadOnlyMemory<byte> payload,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var slowRestoreScenario = spot.Scenario.Contains(
                "SLOW-RESTORE",
                StringComparison.Ordinal)
                ? spot.Scenario
                : spot.Context.SpotId.Contains(
                    "st-g5-instance-spot-slow-restore",
                    StringComparison.Ordinal)
                    ? "ST-G5-INSTANCE-SPOT-SLOW-RESTORE"
                    : null;
            if (slowRestoreScenario is not null)
            {
                evidence.Add(
                    slowRestoreScenario,
                    spot.Context.SpotId,
                    "slow_restore_started",
                    "1250ms");
                await Task.Delay(1_250, cancellationToken)
                    .ConfigureAwait(false);
            }
            spot.ApplicationState = payload.ToArray();
            evidence.Add(
                "ST-I1",
                spot.Context.SpotId,
                "spot_application_state_restored",
                $"kind=instance;bytes={payload.Length};sha256="
                + TransferActorStateCodec.Sha256(payload.Span));
        }
    }

    internal sealed class RelocationPayloadPerActorUserSpotAdapter(
        EvidenceStore evidence)
        : IZLinkSpotRelocationAdapter<
            RelocationPayloadPerActorUserSpot>
    {
        public async ValueTask<byte[]> CaptureAsync(
            RelocationPayloadPerActorUserSpot spot,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (spot.Scenario.Contains(
                    "SLOW-CAPTURE",
                    StringComparison.Ordinal))
            {
                evidence.Add(
                    spot.Scenario,
                    spot.Context.SpotId,
                    "unexpected_spot_slow_capture",
                    "per_actor");
                await Task.Delay(1_250, cancellationToken)
                    .ConfigureAwait(false);
            }
            evidence.Add(
                spot.Scenario,
                spot.Context.SpotId,
                "spot_application_payload",
                $"kind=peractor;bytes={spot.ApplicationState.Length};sha256="
                + TransferActorStateCodec.Sha256(spot.ApplicationState));
            return spot.ApplicationState;
        }

        public async ValueTask RestoreAsync(
            RelocationPayloadPerActorUserSpot spot,
            ReadOnlyMemory<byte> payload,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (spot.Scenario.Contains(
                    "SLOW-RESTORE",
                    StringComparison.Ordinal))
            {
                evidence.Add(
                    spot.Scenario,
                    spot.Context.SpotId,
                    "unexpected_spot_slow_restore",
                    "per_actor");
                await Task.Delay(1_250, cancellationToken)
                    .ConfigureAwait(false);
            }
            spot.ApplicationState = payload.ToArray();
            evidence.Add(
                "ST-G3",
                spot.Context.SpotId,
                "spot_application_state_restored",
                $"kind=peractor;bytes={payload.Length};sha256="
                + TransferActorStateCodec.Sha256(payload.Span));
        }
    }
}

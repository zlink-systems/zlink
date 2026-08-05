// Verifies measured Actor and Spot relocation payload sizes match the configured profiles.
using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;
// Verifies measured Actor and Spot relocation payload sizes match the configured profiles.
using SpotActorTransfer.Client.Support;
using SpotActorTransfer.Shared;
using Zlink.HttpClient;

namespace SpotActorTransfer.Client.Scenarios;

internal static class StI1RelocationPayloadMeasurementScenario
{
    private static readonly (string Name, int ApplicationBytes)[] ActorProfiles =
    [
        ("small", 4 * 1024),
        ("normal", 64 * 1024),
        ("large", 8 * 1024 * 1024),
        ("boundary-single", 64 * 1024 * 1024)
    ];

    private static readonly (string Name, int InstanceBytes, int SpotWideBytes)[]
        SpotProfiles =
    [
        ("small", 64 * 1024, 64 * 1024),
        ("normal", 1024 * 1024, 1024 * 1024),
        ("large", 32 * 1024 * 1024, 32 * 1024 * 1024),
        ("boundary-single", 64 * 1024 * 1024, 64 * 1024 * 1024)
    ];

    public static async Task RunAsync(SpotActorTransferScenarioContext context)
    {
        foreach (var profile in ActorProfiles)
            await RunActorProfileAsync(
                context,
                profile.Name,
                profile.ApplicationBytes);
        await RunSpotProfilesAsync(
            context,
            includeInstance: true,
            includeSpotWide: true);
    }

    internal static Task RunSpotWideRelocationOnlyAsync(
        SpotActorTransferScenarioContext context) =>
        RunSpotProfilesAsync(
            context,
            includeInstance: false,
            includeSpotWide: true);

    internal static Task RunActorBoundaryRelocationOnlyAsync(
        SpotActorTransferScenarioContext context) =>
        RunActorProfileAsync(
            context,
            "boundary-single",
            64 * 1024 * 1024);

    internal static Task RunSpotWideSmallRelocationOnlyAsync(
        SpotActorTransferScenarioContext context) =>
        RunSpotProfilesAsync(
            context,
            includeInstance: false,
            includeSpotWide: true,
            profiles: SpotProfiles.Where(static profile =>
                profile.Name == "small").ToArray());

    internal static Task RunSpotWideBoundaryRelocationOnlyAsync(
        SpotActorTransferScenarioContext context) =>
        RunSpotProfilesAsync(
            context,
            includeInstance: false,
            includeSpotWide: true,
            profiles: SpotProfiles.Where(static profile =>
                profile.Name == "boundary-single").ToArray());

    internal static Task RunInstanceRelocationOnlyAsync(
        SpotActorTransferScenarioContext context) =>
        RunSpotProfilesAsync(
            context,
            includeInstance: true,
            includeSpotWide: false);

    private static async Task RunActorProfileAsync(
        SpotActorTransferScenarioContext context,
        string profile,
        int applicationBytes)
    {
        var scenario = $"ST-I1-{profile}";
        var actorId =
            $"actor-payload-{profile}-{Guid.NewGuid():N}";
        if (profile == "boundary-single")
        {
            // The public 64 MiB bound applies to the bytes returned by the
            // adapter. This fixture adds its own deterministic header.
            applicationBytes = 64 * 1024 * 1024
                - sizeof(uint)
                - sizeof(int)
                - sizeof(int)
                - Encoding.UTF8.GetByteCount(actorId);
        }
        var spotId =
            $"spot-payload-{profile}-{Guid.NewGuid():N}";

        await context.ResetRelocationBlobMeasurementsAsync(
            context.NodeA,
            context.NodeB,
            context.NodeC);
        var created = await context.CreateActorAsync(
            context.NodeA,
            actorId,
            SpotActorTransferNames.ActorTypeStateful,
            stateVersion: applicationBytes,
            applicationStateBytes: applicationBytes);
        var source = context.NodeForRid(created.NodeRid);
        var (target, targetPrefix) =
            context.OtherActorNode(created.NodeRid);
        await context.CreateSpotAsync(target, spotId);

        var join = await context.JoinAsync(
            source,
            actorId,
            new JoinTargetReq(scenario, spotId));
        ZlinkStreamAssert.Ensure(
            join.Accepted,
            $"{scenario} relocation was rejected.");

        var probe = await context.ProbeAsync(
            target,
            actorId,
            new ProbeReq(scenario, "payload-restored"));
        ZlinkStreamAssert.Ensure(
            SpotActorTransferScenarioContext.IsNode(
                probe.NodeRid,
                targetPrefix),
            $"{scenario} restored on unexpected node '{probe.NodeRid}'.");
        ZlinkStreamAssert.Ensure(
            probe.StateVersion == applicationBytes,
            $"{scenario} state version was not restored.");

        var expectedCaptureBytes = checked(
            applicationBytes
            + sizeof(uint)
            + sizeof(int)
            + sizeof(int)
            + Encoding.UTF8.GetByteCount(actorId));
        var expectedApplicationState = CreateExpectedState(
            actorId,
            applicationBytes);
        var expectedApplicationSha256 = Sha256(expectedApplicationState);
        var expectedCaptureSha256 = ExpectedCaptureSha256(
            actorId,
            applicationBytes,
            expectedApplicationState);
        var sourceEvidence = await context.WaitEvidenceAsync(
            source,
            [
                $"transfer|{actorId}|application_payload|"
                + $"bytes={expectedCaptureBytes};sha256={expectedCaptureSha256}"
            ]);
        ZlinkStreamAssert.Ensure(
            sourceEvidence.Count(item =>
                item.ActorId == actorId
                && item.Kind == "application_payload"
                && item.Value == $"bytes={expectedCaptureBytes};"
                    + $"sha256={expectedCaptureSha256}") == 1,
            $"{scenario} did not observe exactly one application payload.");
        var targetEvidence = await context.WaitEvidenceAsync(
            target,
            [
                $"transfer|{actorId}|application_state_restored|"
                + $"bytes={applicationBytes};sha256="
                + expectedApplicationSha256
            ]);
        ZlinkStreamAssert.Ensure(
            targetEvidence.Count(item =>
                item.ActorId == actorId
                && item.Kind == "application_state_restored"
                && item.Value == $"bytes={applicationBytes};"
                    + $"sha256={expectedApplicationSha256}") == 1,
            $"{scenario} restored application bytes do not match the"
            + " deterministic source state.");

        var measurements = new List<RelocationBlobMeasurement>();
        foreach (var node in new[]
                 {
                     context.NodeA,
                     context.NodeB,
                     context.NodeC
                 })
        {
            measurements.AddRange(
                await context.GetRelocationBlobMeasurementsAsync(node));
        }

        var puts = measurements
            .Where(static item => item.Operation == "put")
            .ToArray();
        var reads = measurements
            .Where(static item => item.Operation == "read")
            .ToArray();
        ZlinkStreamAssert.Ensure(
            puts.Length > 0,
            $"{scenario} did not write a Relocation Store blob.");
        ZlinkStreamAssert.Ensure(
            reads.Length > 0,
            $"{scenario} did not read a Relocation Store blob.");
        ZlinkStreamAssert.Ensure(
            puts.Any(put =>
                put.EncodedBytes >= expectedCaptureBytes
                && reads.Any(read =>
                    read.OpaqueReferenceSha256 == put.OpaqueReferenceSha256
                    && read.EncodedBytes == put.EncodedBytes
                    && read.PayloadSha256 == put.PayloadSha256)),
            $"{scenario} did not read back an opaque blob with matching"
            + " size, reference hash, and payload checksum.");

        Console.WriteLine(
            $"{scenario} payload application_bytes={applicationBytes}"
            + $" capture_bytes={expectedCaptureBytes}"
            + $" store_put_bytes={puts.Max(static item => item.EncodedBytes)}"
            + $" store_blob_count={puts.Length}");

        var destroyed = await context.DestroyActorAsync(target, actorId);
        ZlinkStreamAssert.Ensure(
            destroyed.Destroyed,
            $"{scenario} Actor cleanup failed.");
        await context.ClosePayloadSpotAsync(target, spotId);
    }

    private static async Task RunSpotProfilesAsync(
        SpotActorTransferScenarioContext context,
        bool includeInstance,
        bool includeSpotWide,
        IReadOnlyList<(string Name, int InstanceBytes, int SpotWideBytes)>?
            profiles = null)
    {
        const string desiredSource = "actor-a";
        var fixtures = new List<(
            string Kind,
            string Profile,
            string SpotId,
            int Bytes,
            string Sha256)>();

        foreach (var profile in profiles ?? SpotProfiles)
        {
            if (includeInstance)
            {
                fixtures.Add(await CreateOnSourceAsync(
                    context,
                    "instance",
                    profile.Name,
                    profile.InstanceBytes,
                    desiredSource));
            }
            if (includeSpotWide)
            {
                fixtures.Add(await CreateOnSourceAsync(
                    context,
                    "spotwide",
                    profile.Name,
                    profile.SpotWideBytes,
                    desiredSource));
            }
        }

        await context.ResetRelocationBlobMeasurementsAsync(
            context.NodeA,
            context.NodeB,
            context.NodeC);
        var memoryBefore = await ReadPeakWorkingSetAsync(context);
        var relocation = await context.RelocateAsync(
            context.NodeA,
            TimeSpan.FromMinutes(5));
        ZlinkStreamAssert.Ensure(
            relocation.Outcome == "Relocated"
            && relocation.State == "Relocated",
            "ST-I1 Spot payload host relocation did not reach Relocated: "
            + $"{relocation.Outcome}/{relocation.Reason}/{relocation.State}.");

        foreach (var fixture in fixtures)
        {
            var current = fixture.Kind == "instance"
                ? await context.ActivatePayloadInstanceSpotAsync(
                    context.NodeC,
                    fixture.SpotId,
                    new RelocationPayloadSpotReq(
                        "ST-I1",
                        fixture.Bytes))
                : await context.CreatePayloadUserSpotAsync(
                    context.NodeC,
                    fixture.SpotId,
                    new RelocationPayloadSpotReq(
                        "ST-I1",
                        fixture.Bytes));
            ZlinkStreamAssert.Ensure(
                !SpotActorTransferScenarioContext.IsNode(
                    current.NodeRid,
                    desiredSource)
                && current.ApplicationStateBytes == fixture.Bytes
                && current.ApplicationStateSha256 == fixture.Sha256,
                $"ST-I1 {fixture.Kind}/{fixture.Profile} did not preserve "
                + "its application state on a different owner.");
            var target = context.NodeForRid(current.NodeRid);
            await context.WaitEvidenceAsync(
                target,
                [
                    $"ST-I1|{fixture.SpotId}|"
                    + "spot_application_state_restored|"
                    + $"kind={fixture.Kind};bytes={fixture.Bytes};"
                    + $"sha256={fixture.Sha256}"
                ]);
        }

        var measurements = new List<RelocationBlobMeasurement>();
        foreach (var node in new[]
                 {
                     context.NodeA,
                     context.NodeB,
                     context.NodeC
                 })
        {
            measurements.AddRange(
                await context.GetRelocationBlobMeasurementsAsync(node));
        }
        var puts = measurements
            .Where(static item => item.Operation == "put")
            .ToArray();
        var reads = measurements
            .Where(static item => item.Operation == "read")
            .ToArray();
        ZlinkStreamAssert.Ensure(
            puts.Length > 0
            && puts.All(put => reads.Any(read =>
                read.OpaqueReferenceSha256 == put.OpaqueReferenceSha256
                && read.EncodedBytes == put.EncodedBytes
                && read.PayloadSha256 == put.PayloadSha256)),
            "ST-I1 Spot relocation did not read every opaque Store blob "
            + "back with the same size and checksum.");

        var applicationBytes = fixtures.Sum(
            static fixture => (long)fixture.Bytes);
        var storePutBytes = puts.Sum(
            static item => (long)item.EncodedBytes);
        var memoryAfter = await ReadPeakWorkingSetAsync(context);
        Console.WriteLine(
            "ST-I1 spot_payloads"
            + $" fixture_count={fixtures.Count}"
            + $" application_bytes={applicationBytes}"
            + $" store_put_bytes={storePutBytes}"
            + $" opaque_store_overhead_bytes="
            + $"{Math.Max(0, storePutBytes - applicationBytes)}"
            + $" peak_rss_before_bytes={memoryBefore}"
            + $" peak_rss_after_bytes={memoryAfter}");
        Console.WriteLine(
            "ST-I1 blocker"
            + " queue_journal_timer_profile=not_exercised"
            + " permit_before_seal_contention=not_exercised"
            + " aggregate_320_mib_five_participants=not_exercised");

        foreach (var fixture in fixtures)
            if (fixture.Kind != "instance")
                await context.ClosePayloadSpotAsync(
                    context.NodeC,
                    fixture.SpotId);
    }

    private static async Task<(
        string Kind,
        string Profile,
        string SpotId,
        int Bytes,
        string Sha256)> CreateOnSourceAsync(
        SpotActorTransferScenarioContext context,
        string kind,
        string profile,
        int bytes,
        string desiredSource)
    {
        var placementNode = desiredSource switch
        {
            "actor-a" => context.NodeA,
            "actor-b" => context.NodeB,
            "actor-c" => context.NodeC,
            _ => throw new ArgumentOutOfRangeException(nameof(desiredSource))
        };
        return await context.WithPlacementNodeAsync(
            placementNode,
            async () =>
            {
            var spotId =
                $"payload-{kind}-{profile}-{Guid.NewGuid():N}";
            var request = new RelocationPayloadSpotReq(
                $"ST-I1-{kind}-{profile}",
                bytes);
            var created = kind == "instance"
                ? await context.ActivatePayloadInstanceSpotAsync(
                    placementNode,
                    spotId,
                    request)
                : await context.CreatePayloadUserSpotAsync(
                    placementNode,
                    spotId,
                    request);
            ZlinkStreamAssert.Ensure(
                SpotActorTransferScenarioContext.IsNode(
                    created.NodeRid,
                    desiredSource),
                $"ST-I1 {kind}/{profile} placement weight did not select "
                + $"{desiredSource}.");
            return (
                kind,
                profile,
                spotId,
                bytes,
                created.ApplicationStateSha256);
            });
    }

    private static async Task<long> ReadPeakWorkingSetAsync(
        SpotActorTransferScenarioContext context)
    {
        var snapshots = await Task.WhenAll(
            context.GetProcessMemoryAsync(context.NodeA),
            context.GetProcessMemoryAsync(context.NodeB),
            context.GetProcessMemoryAsync(context.NodeC));
        return snapshots.Max(static snapshot => snapshot.PeakWorkingSetBytes);
    }

    private static byte[] CreateExpectedState(string actorId, int size)
    {
        var result = new byte[size];
        if (result.Length == 0)
            return result;
        var seed = SHA256.HashData(Encoding.UTF8.GetBytes(actorId));
        var state = BinaryPrimitives.ReadUInt64LittleEndian(seed);
        for (var index = 0; index < result.Length; index++)
        {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            result[index] = (byte)state;
        }
        return result;
    }

    private static string ExpectedCaptureSha256(
        string actorId,
        int stateVersion,
        byte[] applicationState)
    {
        const uint magic = 0x5a4c5331;
        var actorIdBytes = Encoding.UTF8.GetBytes(actorId);
        var payload = new byte[
            sizeof(uint)
            + sizeof(int)
            + sizeof(int)
            + actorIdBytes.Length
            + applicationState.Length];
        BinaryPrimitives.WriteUInt32LittleEndian(payload, magic);
        BinaryPrimitives.WriteInt32LittleEndian(
            payload.AsSpan(sizeof(uint)),
            stateVersion);
        BinaryPrimitives.WriteInt32LittleEndian(
            payload.AsSpan(sizeof(uint) + sizeof(int)),
            actorIdBytes.Length);
        actorIdBytes.CopyTo(
            payload.AsSpan(sizeof(uint) + sizeof(int) + sizeof(int)));
        applicationState.CopyTo(
            payload.AsSpan(
                sizeof(uint)
                + sizeof(int)
                + sizeof(int)
                + actorIdBytes.Length));
        return Sha256(payload);
    }

    private static string Sha256(ReadOnlySpan<byte> payload) =>
        Convert.ToHexString(SHA256.HashData(payload)).ToLowerInvariant();
}

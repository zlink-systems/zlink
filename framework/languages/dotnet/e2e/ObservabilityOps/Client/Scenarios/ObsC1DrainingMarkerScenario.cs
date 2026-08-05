// Verifies OBS-C1 Draining Marker behavior.
using ObservabilityOps.Client.Support;
using ObservabilityOps.Shared;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.HttpClient;

namespace ObservabilityOps.Client.Scenarios;

internal static class ObsC1DrainingMarkerScenario
{
    public static async Task RunAsync(ScenarioContext context)
    {
        _ = await context.PlayNodeIdAsync("play-a");
        _ = await context.PlayNodeIdAsync("play-b");
        var suffix = Guid.NewGuid().ToString("N");
        var actorId = $"obs-c1-{suffix}";
        var roomPrefix = $"room-c1-{suffix}";
        // The scenario reads host-state metrics from the node it relocates, and
        // play-b runs with metrics disabled on purpose for OBS-B4, so the room
        // has to sit on play-a.
        var room = await context.CreateRoomOnObservedNodeAsync("play-a", roomPrefix);
        var roomRid = room.RoomRid;
        RoomRid = roomRid;
        var source = context.Play(room.NodeRid);
        await using var connector = await context.ConnectAsync();
        await connector.Request(new AuthenticateReq(actorId)).Async<AuthenticateRes>();
        await context.JoinRoomAsync(connector, actorId, roomRid);
        await source.Post("/relocate?deadlineMs=30000").AsyncRaw();

        //  Config 11 OBS-C1이 요구하는 것은 host status의 `State=Relocating`,
        //  `IsReady=false`와 신규 배치 제외이지 peer가 관측하는 Draining 표시가
        //  아니다. 그리고 이 evidence는 relocate 대상인 `source` 자신에게서 읽으므로
        //  구조적으로 peer의 Draining이 나올 수 없다. `source`는 자기 자신을 자기
        //  peer 목록에 두지 않는다. Spec이 요구하는 조건만 기다린다.
        var draining = await WaitEvidenceAsync(source, snapshot =>
            !snapshot.Ready
            && snapshot.SpotRows.Any(row => row.SpotRid == roomRid)
            // The relocating state can pass faster than any snapshot poll, so
            // spec 24 §3 exposes state changes as a stream. The host records
            // every observed status, and this reads that record instead of
            // racing the live gauge.
            && snapshot.Entries.Any(line =>
                line.Contains("host-state|", StringComparison.Ordinal)
                && line.Contains("state=Relocating", StringComparison.Ordinal)));
        ZlinkStreamAssert.Ensure(
            draining.Entries.Any(line =>
                line.Contains("host-state|", StringComparison.Ordinal)
                && line.Contains("state=Relocating", StringComparison.Ordinal)),
            "OBS-C1 host did not publish the Relocating state.");
        //  Config 11은 OBS-C1의 몫을 **배치 제외**로, bound-session 연속성을
        //  OBS-C2의 몫으로 명시한다(config-11 §296~297). 이전에는 여기서
        //  relocate 시작 뒤 새로 보낸 bound-session 요청의 성공을 단언했는데,
        //  그것은 C2의 검증이고 C1이 요구하는 "이미 수락한 작업"도 아니다.
        //  C1이 실제로 요구하는 것만 확인한다.
        //  요청은 draining이 아닌 host에서 낸다. Spec 28 §11 step 1이 draining
        //  host의 신규 admission을 닫으므로, relocate 중인 play-a에 직접 내면
        //  placement에 닿기 전에 `RequireSpotAdmission`이 거절한다(그게 정상이다).
        //  "배치 제외"는 **다른 host가 target으로 고르지 않는다**는 뜻이다.
        var relocatingNode = room.NodeRid;
        for (var attempt = 0; attempt < 8; attempt++)
        {
            var placed = (await context.PlayB.Post("/rooms")
                .Body(new CreateRoomReq($"obs-c1-exclusion-{attempt}-{Guid.NewGuid():N}", "normal"))
                .Async<CreateRoomRes>()).Body;
            ZlinkStreamAssert.Ensure(
                placed.NodeRid != relocatingNode,
                "OBS-C1 relocating host was still selected for a new room placement.");
            await context.PlayB.Post($"/rooms/{placed.RoomRid}/close").AsyncRaw();
        }
        ZlinkStreamAssert.Ensure(
            (await EvidenceAsync(source)).SpotRows.Any(row => row.SpotRid == roomRid),
            "OBS-C1 room disappeared before relocation commit.");
        await connector.Close.Async();
        Console.WriteLine("scenario OBS-C1 passed");
    }

    private static async Task<EvidenceSnapshot> WaitEvidenceAsync(
        ZLinkHttpClient source,
        Func<EvidenceSnapshot, bool> predicate)
    {
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(10);
        while (DateTimeOffset.UtcNow < deadline)
        {
            var snapshot = await EvidenceAsync(source);
            if (predicate(snapshot)) return snapshot;
            await Task.Delay(100);
        }
        throw new TimeoutException("OBS-C1 draining evidence did not converge.");
    }

    private static async Task<EvidenceSnapshot> EvidenceAsync(ZLinkHttpClient source) =>
        (await source.Get("/evidence").Query("spotRid", RoomRid!)
            .Async<EvidenceSnapshot>()).Body;

    private static string? RoomRid;
}

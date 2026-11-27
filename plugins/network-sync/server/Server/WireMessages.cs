using System.Text.Json.Serialization;
using Zlink.Framework.Contracts.Handlers;
using Zlink.NetworkSync.Simulation;

namespace Zlink.NetworkSync.Server;

[ZLinkPacket(NetworkSyncSession.InputPacket)]
internal sealed record NetworkSyncInputV1<TInput, TAction>(
    [property: JsonPropertyName("match_id")] string MatchId,
    ulong Epoch,
    [property: JsonPropertyName("target_tick")] ulong TargetTick,
    [property: JsonPropertyName("t1_us")] ulong T1Us,
    [property: JsonPropertyName("last_snapshot_tick")] ulong? LastSnapshotTick,
    TInput Input,
    ActionV1<TAction>[] Actions);

internal readonly record struct ActionV1<TAction>(
    [property: JsonPropertyName("action_id")] ulong Id,
    TAction Payload);

internal sealed record NetworkSyncPingV1(
    [property: JsonPropertyName("match_id")] string MatchId,
    [property: JsonPropertyName("probe_id")] ulong ProbeId,
    [property: JsonPropertyName("t1_us")] ulong T1Us);

[ZLinkPacket("NetworkSyncSnapshotV1")]
internal sealed record NetworkSyncSnapshotV1<TState>(
    AnchorV1 Anchor,
    TState State,
    [property: JsonPropertyName("last_applied_input_tick")] ulong? LastAppliedInputTick,
    [property: JsonPropertyName("used_fallback_input")] bool UsedFallbackInput,
    [property: JsonPropertyName("input_feedback")] FeedbackV1? InputFeedback);

internal sealed record NetworkSyncPongV1(
    AnchorV1 Anchor,
    [property: JsonPropertyName("probe_id")] ulong ProbeId,
    [property: JsonPropertyName("t1_us")] ulong T1Us,
    [property: JsonPropertyName("hold_us")] ulong HoldUs);

internal readonly record struct AnchorV1(
    [property: JsonPropertyName("match_id")] string MatchId,
    ulong Epoch,
    ulong Tick,
    [property: JsonPropertyName("tick_us")] ulong TickUs,
    [property: JsonPropertyName("since_tick_us")] ulong SinceTickUs);

internal readonly record struct FeedbackV1(
    [property: JsonPropertyName("target_tick")] ulong TargetTick,
    [property: JsonPropertyName("t1_us")] ulong T1Us,
    [property: JsonPropertyName("hold_us")] ulong HoldUs,
    [property: JsonPropertyName("arrival_margin_us")] long ArrivalMarginUs,
    WireInputDisposition Disposition);

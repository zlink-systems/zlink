// SPDX-License-Identifier: LicenseRef-Zlink
#nullable enable
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Zlink.NetworkSync
{
    internal static class NetworkSyncPackets
    {
        internal const string Input = "NetworkSyncInputV1";
        internal const string Ping = "NetworkSyncPingV1";
        internal const string Snapshot = "NetworkSyncSnapshotV1";
        internal const string Pong = "NetworkSyncPongV1";
    }

    internal enum InputDisposition
    {
        Accepted,
        Late,
        Duplicate,
        Rejected,
    }

    internal sealed class NetworkSyncInputV1<TInput, TAction>
    {
        [JsonRequired, JsonPropertyName("match_id")]
        public string MatchId { get; set; } = null!;

        [JsonRequired, JsonPropertyName("epoch")]
        public ulong Epoch { get; set; }

        [JsonRequired, JsonPropertyName("target_tick")]
        public ulong TargetTick { get; set; }

        [JsonRequired, JsonPropertyName("t1_us")]
        public ulong T1Us { get; set; }

        [JsonRequired, JsonPropertyName("last_snapshot_tick")]
        public ulong? LastSnapshotTick { get; set; }

        [JsonRequired, JsonPropertyName("input")]
        public TInput? Input { get; set; }

        [JsonRequired, JsonPropertyName("actions")]
        public NetworkSyncAction<TAction>[] Actions { get; set; } = null!;
    }

    internal sealed class NetworkSyncPingV1
    {
        [JsonRequired, JsonPropertyName("match_id")]
        public string MatchId { get; set; } = null!;

        [JsonRequired, JsonPropertyName("probe_id")]
        public ulong ProbeId { get; set; }

        [JsonRequired, JsonPropertyName("t1_us")]
        public ulong T1Us { get; set; }
    }

    internal sealed class NetworkSyncSnapshotV1<TState>
    {
        [JsonRequired, JsonPropertyName("anchor")]
        public MatchTimelineAnchor Anchor { get; set; } = null!;

        [JsonRequired, JsonPropertyName("state")]
        public TState? State { get; set; }

        [JsonRequired, JsonPropertyName("last_applied_input_tick")]
        public ulong? LastAppliedInputTick { get; set; }

        [JsonRequired, JsonPropertyName("used_fallback_input")]
        public bool UsedFallbackInput { get; set; }

        [JsonRequired, JsonPropertyName("input_feedback")]
        public InputFeedback? InputFeedback { get; set; }
    }

    internal sealed class NetworkSyncPongV1
    {
        [JsonRequired, JsonPropertyName("anchor")]
        public MatchTimelineAnchor Anchor { get; set; } = null!;

        [JsonRequired, JsonPropertyName("probe_id")]
        public ulong ProbeId { get; set; }

        [JsonRequired, JsonPropertyName("t1_us")]
        public ulong T1Us { get; set; }

        [JsonRequired, JsonPropertyName("hold_us")]
        public ulong HoldUs { get; set; }
    }

    internal sealed class NetworkSyncAction<TAction>
    {
        [JsonRequired, JsonPropertyName("action_id")]
        public ulong ActionId { get; set; }

        [JsonRequired, JsonPropertyName("payload")]
        public TAction? Payload { get; set; }
    }

    internal sealed class MatchTimelineAnchor
    {
        private ulong _tickUs;

        [JsonRequired, JsonPropertyName("match_id")]
        public string MatchId { get; set; } = null!;

        [JsonRequired, JsonPropertyName("epoch")]
        public ulong Epoch { get; set; }

        [JsonRequired, JsonPropertyName("tick")]
        public ulong Tick { get; set; }

        [JsonRequired, JsonPropertyName("tick_us")]
        public ulong TickUs
        {
            get => _tickUs;
            set => _tickUs = value > 0 ? value : throw new JsonException("tick_us must be positive.");
        }

        [JsonRequired, JsonPropertyName("since_tick_us")]
        public ulong SinceTickUs { get; set; }
    }

    internal sealed class InputFeedback
    {
        [JsonRequired, JsonPropertyName("target_tick")]
        public ulong TargetTick { get; set; }

        [JsonRequired, JsonPropertyName("t1_us")]
        public ulong T1Us { get; set; }

        [JsonRequired, JsonPropertyName("hold_us")]
        public ulong HoldUs { get; set; }

        [JsonRequired, JsonPropertyName("arrival_margin_us")]
        public long ArrivalMarginUs { get; set; }

        [JsonRequired, JsonPropertyName("disposition")]
        public InputDisposition Disposition { get; set; }
    }
}

using MyGame.Channel.Domain;
using MyGame.Shared;

namespace MyGame.E2E;

internal static class DomainChecks
{
    public static void Run()
    {
        var checks = 0;
        void Check(bool condition, string message)
        {
            checks++;
            if (!condition)
                throw new InvalidOperationException(message);
        }

        var clock = new ManualTimeProvider();
        var gold = new GoldBalance(0, clock);
        var accepted = gold.EvaluateEarn();
        Check(accepted.Accepted && accepted.NewValue == GameRules.EarnAmount, "Initial earn was not accepted.");
        Check(gold.Value == 0, "Gold changed before persistence completed.");
        gold.CommitEarn(accepted);
        Check(gold.Value == GameRules.EarnAmount, "Accepted gold was not committed.");

        var rejected = gold.EvaluateEarn();
        Check(
            rejected.Reason == Rejection.EarnTooSoon
                && rejected.RetryAfterMs == GameRules.EarnIntervalMs,
            "Immediate earn did not return the full cooldown."
        );
        Check(
            gold.Value == GameRules.EarnAmount
                && gold.Capture().EarnCooldownRemainingMs == GameRules.EarnIntervalMs,
            "Rejected earn changed gold or cooldown state."
        );

        clock.Advance(TimeSpan.FromMilliseconds(250));
        var snapshot = gold.Capture();
        Check(snapshot.EarnCooldownRemainingMs == 750, "Snapshot did not store remaining milliseconds.");
        var relocatedClock = new ManualTimeProvider(TimeSpan.FromHours(1).Ticks);
        var relocated = new GoldBalance(snapshot.Value, relocatedClock);
        relocated.Restore(snapshot);
        Check(
            relocated.EvaluateEarn().RetryAfterMs == 750,
            "Relocation did not restore the remaining cooldown on a new clock origin."
        );
        relocatedClock.Advance(TimeSpan.FromMilliseconds(750));
        var afterCooldown = relocated.EvaluateEarn();
        Check(
            afterCooldown.Accepted && afterCooldown.NewValue == GameRules.EarnAmount * 2,
            "Earn was not accepted after the restored cooldown."
        );

        var boundaryClock = new ManualTimeProvider();
        var atBoundary = new GoldBalance(GameRules.MaxInteger - GameRules.EarnAmount, boundaryClock);
        var boundaryEarn = atBoundary.EvaluateEarn();
        Check(
            boundaryEarn.Accepted && boundaryEarn.NewValue == GameRules.MaxInteger,
            "Gold at maximum minus the reward was not accepted exactly at the limit."
        );
        atBoundary.CommitEarn(boundaryEarn);
        boundaryClock.Advance(TimeSpan.FromMilliseconds(GameRules.EarnIntervalMs));
        var atLimit = atBoundary.EvaluateEarn();
        Check(atLimit.Reason == Rejection.GoldLimit && atLimit.RetryAfterMs == 0, "Gold limit was not rejected.");
        Check(atBoundary.Value == GameRules.MaxInteger, "Gold limit rejection changed the balance.");

        var overBoundary = new GoldBalance(
            GameRules.MaxInteger - GameRules.EarnAmount + 1,
            new ManualTimeProvider()
        );
        var overBoundaryDecision = overBoundary.EvaluateEarn();
        Check(
            overBoundaryDecision.Reason == Rejection.GoldLimit && overBoundaryDecision.RetryAfterMs == 0,
            "Gold at maximum minus reward plus one was not rejected."
        );
        Check(
            overBoundary.Value == GameRules.MaxInteger - GameRules.EarnAmount + 1,
            "Rejected boundary earn changed the balance."
        );

        Console.WriteLine($"Domain checks: {checks}/{checks} passed");
    }

    private sealed class ManualTimeProvider(long timestamp = 0) : TimeProvider
    {
        private long _timestamp = timestamp;

        public override long TimestampFrequency => TimeSpan.TicksPerSecond;

        public override long GetTimestamp() => _timestamp;

        public void Advance(TimeSpan elapsed) => _timestamp += elapsed.Ticks;
    }
}

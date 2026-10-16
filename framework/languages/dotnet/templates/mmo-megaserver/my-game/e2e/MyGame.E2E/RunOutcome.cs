namespace MyGame.E2E;

internal sealed class RunOutcome
{
    // Carry this run's single failure owner through existing async resource scopes.
    private static readonly AsyncLocal<RunOutcome?> Current = new();
    private readonly List<Exception> failures = [];

    public static void Record(Exception error) => Current.Value!.failures.Add(error);

    public static void Cleanup(Action cleanup)
    {
        try
        {
            cleanup();
        }
        catch (Exception error)
        {
            Record(error);
        }
    }

    public static async ValueTask CleanupAsync(Func<ValueTask> cleanup)
    {
        try
        {
            await cleanup();
        }
        catch (Exception error)
        {
            Record(error);
        }
    }

    public static async Task<int> RunAsync(Func<Task<int>> scenario)
    {
        var previous = Current.Value;
        var outcome = new RunOutcome();
        Current.Value = outcome;
        var result = 0;
        try
        {
            result = await scenario();
        }
        catch (Exception error)
        {
            outcome.failures.Insert(0, error);
        }
        finally
        {
            Current.Value = previous;
        }
        if (outcome.failures.Count > 0)
            throw new AggregateException(
                "Run failed (scenario first, then cleanup).",
                outcome.failures
            );
        return result;
    }
}

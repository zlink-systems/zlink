namespace MyGame.E2E;

internal sealed class RunOutcome
{
    public List<Exception> Failures { get; } = [];

    public void Record(Exception error) => Failures.Add(error);

    public void Cleanup(Action cleanup)
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

    public async ValueTask CleanupAsync(Func<ValueTask> cleanup)
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
}

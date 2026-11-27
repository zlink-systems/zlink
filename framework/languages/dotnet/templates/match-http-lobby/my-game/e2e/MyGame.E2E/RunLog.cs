namespace MyGame.E2E;

internal sealed class RunLog
{
    public string DirectoryPath { get; }
    private readonly string[] selected;
    private string? current;
    private int passed;

    public RunLog(string root, string[] selected)
    {
        this.selected = selected;
        DirectoryPath = Path.Combine(root, ".logs", "run-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(DirectoryPath);
        Console.WriteLine($"Run logs: {DirectoryPath}");
    }

    public void Start(string scenario)
    {
        if (current == scenario)
            return;
        current = scenario;
        Console.WriteLine(
            $"━━ {scenario} ({Array.IndexOf(selected, scenario) + 1}/{selected.Length}) ━━"
        );
    }

    public void Pass(string scenario)
    {
        Console.WriteLine($"✓ {scenario}");
        passed++;
        current = null;
    }

    public void Fail(Exception error)
    {
        if (current is null && passed < selected.Length)
            Start(selected[passed]);
        var name = current ?? "run";
        var path = Path.Combine(DirectoryPath, name + "-failure.log");
        File.WriteAllText(path, error.ToString());
        Console.WriteLine(
            $"✗ {name}: {error.Message.Split(['\r', '\n'])[0]} (.logs/{Path.GetFileName(DirectoryPath)}/{Path.GetFileName(path)})"
        );
    }

    public void Complete()
    {
        if (selected.Length > 0)
            Console.WriteLine($"Scenarios: {passed}/{selected.Length} passed");
    }

    public static string[] ForAttached(string[] selected, Func<string, string?> isolationReason)
    {
        return selected
            .Where(scenario =>
            {
                if (isolationReason(scenario) is not { } reason)
                    return true;
                Console.WriteLine($"Skipping {scenario}: {reason} requires an isolated server.");
                return false;
            })
            .ToArray();
    }
}

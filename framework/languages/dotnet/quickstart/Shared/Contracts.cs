namespace QuickStart.Shared;

// Request/reply contract shared by the server and client processes.
public sealed record Hello(string Name);

public sealed record Greeting(string Text);

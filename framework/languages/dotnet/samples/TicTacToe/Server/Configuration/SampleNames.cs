namespace TicTacToe.Server.Configuration;

internal static class SampleChannels
{
    public const string Api = "tictactoe.api";
}

internal static class SampleTypes
{
    public const string PlayerActor = "player";
    public const string GameSpot = "tictactoe-game";
}

internal static class SampleDefaults
{
    public const string GameName = "tictactoe-game";
    public const int RequiredLevel = 3;
}

internal static class SampleNodes
{
    public const string Mesh = "tictactoe";
    public const string ClientStream = "client-stream";
}

internal static class SampleTopics
{
    public const string PlayerMilestoneChannel = "tictactoe.player.milestone.channel";
    public const string PlayerMilestone = "tictactoe.player.milestone";
}

internal static class SampleTimeouts
{
    public static TimeSpan Request { get; } = TimeSpan.FromSeconds(5);
}

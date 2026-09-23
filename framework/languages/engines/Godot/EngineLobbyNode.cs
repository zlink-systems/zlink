using Godot;

namespace EngineLobby;

public partial class EngineLobbyNode : Control
{
    [Export]
    public string Endpoint { get; set; } = "ws://127.0.0.1:22700";

    [Export]
    public string PlayerName { get; set; } = "godot-player";

    [Export]
    public string FirstChat { get; set; } = "hello from Godot";

    private EngineLobbyClient? _client;
    private Label _status = null!;
    private bool _dispatchInFlight;
    private bool _exiting;
    private bool _chatShown;

    public override async void _Ready()
    {
        _status = GetNode<Label>("Status");
        try
        {
            _client = new EngineLobbyClient(Endpoint);
            _client.ChatReceived += OnChatReceived;
            await _client.ConnectAndJoinAsync(PlayerName);
            if (!_exiting && !_chatShown)
                _status.Text = $"joined as {_client.Joined!.Name} ({_client.Joined.ActorId})";
            await _client.SendChatAsync(FirstChat);
        }
        catch (Exception error)
        {
            if (!_exiting)
                _status.Text = $"Engine Lobby failed: {error.Message}";
            GD.PushError(error.ToString());
        }
    }

    public override void _Process(double delta)
    {
        if (_exiting || _dispatchInFlight || _client is not { IsConnected: true })
            return;

        _dispatchInFlight = true;
        DispatchOnceAsync();
    }

    private async void DispatchOnceAsync()
    {
        try
        {
            await _client!.DispatchAsync();
        }
        catch (Exception error)
        {
            if (!_exiting)
                _status.Text = $"Engine Lobby dispatch failed: {error.Message}";
            GD.PushError(error.ToString());
        }
        finally
        {
            _dispatchInFlight = false;
        }
    }

    private void OnChatReceived(ChatNotify chat)
    {
        _chatShown = true;
        if (!_exiting)
            _status.Text = $"{chat.Name}: {chat.Text}";
    }

    public override async void _ExitTree()
    {
        _exiting = true;
        if (_client is null)
            return;

        _client.ChatReceived -= OnChatReceived;
        try
        {
            await _client.DisposeAsync();
        }
        catch (Exception error)
        {
            GD.PushError(error.ToString());
        }
        finally
        {
            _client = null;
        }
    }
}

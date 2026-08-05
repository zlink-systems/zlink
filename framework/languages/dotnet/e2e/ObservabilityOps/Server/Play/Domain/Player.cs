namespace ObservabilityOps.Server.Play.Domain;

internal sealed class Player(string playerId)
{
    public string PlayerId { get; } = playerId;

    public string RoomRid { get; private set; } = string.Empty;

    public void JoinRoom(string roomRid)
    {
        if (string.IsNullOrWhiteSpace(roomRid))
            throw new InvalidOperationException("Room routing id is required.");

        RoomRid = roomRid;
    }

    public void ReturnToLobby()
    {
        RoomRid = string.Empty;
    }
}

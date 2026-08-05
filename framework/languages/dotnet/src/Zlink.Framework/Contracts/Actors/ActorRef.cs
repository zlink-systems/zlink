namespace Systems.Zlink;

/// <summary>
/// Identifies one exact Actor incarnation and the owner route observed with it.
/// </summary>
public readonly record struct ActorRef(
    string ActorId,
    ulong ObjectGeneration,
    string MeshName,
    RoutingId NodeRid)
{
    private readonly string _actorId = ValidateActorId(ActorId);
    private readonly ulong _objectGeneration = ValidateObjectGeneration(ObjectGeneration);
    private readonly string _meshName = ValidateMeshName(MeshName);
    private readonly RoutingId _nodeRid = ValidateNodeRid(NodeRid);

    public string ActorId
    {
        get => _actorId;
        init => _actorId = ValidateActorId(value);
    }

    public ulong ObjectGeneration
    {
        get => _objectGeneration;
        init => _objectGeneration = ValidateObjectGeneration(value);
    }

    public string MeshName
    {
        get => _meshName;
        init => _meshName = ValidateMeshName(value);
    }

    public RoutingId NodeRid
    {
        get => _nodeRid;
        init => _nodeRid = ValidateNodeRid(value);
    }

    private static string ValidateActorId(string value)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(value);
        if (System.Text.Encoding.UTF8.GetByteCount(value) > 255)
            throw new ArgumentOutOfRangeException(nameof(value));
        return value;
    }

    private static ulong ValidateObjectGeneration(ulong value)
    {
        if (value is 0 or > long.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(value));
        return value;
    }

    private static string ValidateMeshName(string value)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(value);
        return value;
    }

    private static RoutingId ValidateNodeRid(RoutingId value)
    {
        if (value.IsEmpty)
            throw new ArgumentException(
                "Actor owner routing id must not be empty.",
                nameof(value));
        return value;
    }
}

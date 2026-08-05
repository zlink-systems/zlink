using SupportChat.Shared.Contracts;
using Zlink.Framework.Contracts.Actors;

namespace SupportChat.Server.Configuration;

using Systems.Zlink;
public sealed record SupportUserActorCreateReq(
    string ActorId,
    string DisplayName,
    string Role,
    string ParticipantId);

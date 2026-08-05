// Owns actor identities and stream connections shared by actor turn scenarios.
namespace AutomaticTurnDispatch.Client.Scenarios;

internal sealed record AwaitActorScenarioContext(string SpotRid, string ActorA, string ActorB);

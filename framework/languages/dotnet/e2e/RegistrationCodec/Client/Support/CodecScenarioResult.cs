using RegistrationCodec.Shared;

namespace RegistrationCodec.Client.Support;

internal sealed record CodecScenarioRes(EchoRes Json, string ProtobufValue, string MessagePackValue);
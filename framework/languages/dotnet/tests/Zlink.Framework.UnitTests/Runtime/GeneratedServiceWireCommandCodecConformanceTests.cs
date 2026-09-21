using System.Text.Json;
using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class GeneratedServiceWireCommandCodecConformanceTests
{
    private static readonly ServiceWireCodec.DecodeContext Context = new(
        null,
        null,
        null,
        4294966774L,
        uint.MaxValue
    );

    [Fact]
    public void Generated_codec_conforms_to_every_indexed_vector()
    {
        using var index = ReadJson("generated/fixtures/index.json");
        Assert.Equal(3, index.RootElement.GetProperty("version").GetInt32());
        Assert.Equal(9, index.RootElement.GetProperty("fixtures").GetArrayLength());

        var cases = new List<ConformanceCase>();
        foreach (var indexed in index.RootElement.GetProperty("fixtures").EnumerateArray())
        {
            using var fixture = ReadJson(indexed.GetProperty("goldenFixture").GetString()!);
            var fixtureRoot = fixture.RootElement.Clone();
            var surface = indexed.GetProperty("surface").Clone();
            var kind = indexed.GetProperty("kind").GetString()!;

            foreach (var canonical in indexed.GetProperty("canonical").EnumerateArray())
            {
                var vector = canonical.Clone();
                var label =
                    $"canonical:{surface.GetProperty("format").GetString()}:"
                    + vector.GetProperty("name").GetString();
                cases.Add(
                    new(
                        label,
                        "accept",
                        () => AssertCanonical(kind, surface, fixtureRoot, vector, label)
                    )
                );
            }

            foreach (var malformed in indexed.GetProperty("malformed").EnumerateArray())
            {
                var vector = malformed.Clone();
                var label =
                    $"malformed:{surface.GetProperty("format").GetString()}:"
                    + vector.GetProperty("name").GetString();
                cases.Add(
                    new(label, "reject", () => DecodeMalformed(surface, fixtureRoot, vector))
                );
            }
        }

        foreach (
            var operationCase in index.RootElement.GetProperty("operationCases").EnumerateArray()
        )
        {
            var vector = operationCase.Clone();
            var label =
                $"operation:{vector.GetProperty("operation").GetString()}:"
                + vector.GetProperty("name").GetString();
            cases.Add(
                new(
                    label,
                    vector.GetProperty("expect").GetString()!,
                    () => ExerciseOperation(vector)
                )
            );
        }

        Assert.Equal(102, cases.Count);
        var operationCases = index
            .RootElement.GetProperty("operationCases")
            .EnumerateArray()
            .ToArray();
        Assert.Equal(
            29,
            operationCases.Count(item => item.GetProperty("expect").GetString() == "accept")
        );
        Assert.Equal(
            50,
            operationCases.Count(item => item.GetProperty("expect").GetString() == "reject")
        );
        Assert.Equal(
            25,
            operationCases
                .Select(item => item.GetProperty("operation").GetString())
                .Distinct()
                .Count()
        );
        foreach (
            var operation in operationCases.GroupBy(item =>
                item.GetProperty("operation").GetString()!
            )
        )
        {
            var boundary = operation
                .Where(item => item.TryGetProperty("boundaryPair", out _))
                .ToArray();
            Assert.Contains(boundary, item => item.GetProperty("expect").GetString() == "accept");
            Assert.Contains(boundary, item => item.GetProperty("expect").GetString() == "reject");
        }
        foreach (var testCase in cases)
        {
            Assert.True(testCase.Expect is "accept" or "reject", testCase.Label);
            var exception = Record.Exception(testCase.Exercise);
            Assert.False(
                exception is ConformanceHarnessException,
                $"{testCase.Label}: {exception}"
            );
            Assert.True(
                testCase.Expect == "reject" ? exception is not null : exception is null,
                $"{testCase.Label}: {exception}"
            );
        }
    }

    private static void AssertCanonical(
        string kind,
        JsonElement surface,
        JsonElement fixture,
        JsonElement vector,
        string label
    )
    {
        if (kind == "command")
        {
            var frames = Frames(fixture, vector);
            Assert.Equal(
                frames,
                GeneratedCommandRoundTrip(surface.GetProperty("commandId").GetInt32(), frames)
            );
            Assert.Equal(
                frames,
                PilotCommandRoundTrip(surface.GetProperty("commandId").GetInt32(), frames)
            );
            return;
        }

        var property = kind == "logical" ? "logicalHex" : "encodedHex";
        var bytes = Convert.FromHexString(
            Pointer(fixture, vector.GetProperty("pointers").GetProperty(property)).GetString()!
        );
        Assert.Equal(
            bytes,
            GeneratedFormatRoundTrip(surface.GetProperty("format").GetString()!, bytes)
        );

        var oracle = PilotFormatRoundTrip(surface.GetProperty("format").GetString()!, bytes);
        if (oracle is not null)
            Assert.True(bytes.SequenceEqual(oracle), $"{label}:oracle");
    }

    private static void DecodeMalformed(
        JsonElement surface,
        JsonElement fixture,
        JsonElement vector
    )
    {
        var commandId = surface.GetProperty("commandId").GetInt32();
        var frames = Frames(fixture, vector);
        var generatedRejected =
            Record.Exception(() => GeneratedCommandRoundTrip(commandId, frames)) is not null;
        var pilotRejected =
            Record.Exception(() => PilotCommandRoundTrip(commandId, frames)) is not null;
        if (generatedRejected && pilotRejected)
            throw new InvalidDataException("generated and pilot codecs rejected the vector");
    }

    private static void ExerciseOperation(JsonElement vector)
    {
        var expect = vector.GetProperty("expect").GetString()!;
        var directions = vector.TryGetProperty("directions", out var declared)
            ? declared.EnumerateArray().Select(item => item.GetString()!).ToArray()
            : ["decode"];
        var wire = OperationWire(vector);
        foreach (var direction in directions)
        {
            var exception = Record.Exception(() => ExerciseDirection(vector, direction, wire));
            if (expect == "accept" && exception is not null)
                throw new ConformanceHarnessException($"{direction} rejected: {exception}");
            if (expect == "reject" && exception is null)
                throw new ConformanceHarnessException($"{direction} accepted");
            if (exception is ConformanceHarnessException)
                throw exception;
        }

        if (expect == "reject")
            throw new InvalidDataException("all declared directions rejected");
    }

    private static void ExerciseDirection(JsonElement vector, string direction, object wire)
    {
        var name = vector.GetProperty("name").GetString()!;
        if (vector.GetProperty("surface").GetProperty("format").GetString() == "semantic")
        {
            var input = vector.GetProperty("input");
            ServiceWireCodec.ValidateTerminalFailure(
                Enum.Parse<ServiceWireCodec.RequestTerminalResult>(
                    Pascal(input.GetProperty("terminalResult").GetString()!)
                ),
                Enum.Parse<ServiceWireCodec.FrameworkErrorCode>(
                    Pascal(input.GetProperty("failureCode").GetString()!)
                )
            );
            return;
        }

        var context = ContextFor(vector);
        if (direction == "decode")
        {
            DecodeSurface(vector, wire, context);
            return;
        }
        if (direction != "encode")
            throw new ConformanceHarnessException($"unknown direction {name}:{direction}");

        var value = vector.TryGetProperty("input", out _)
            ? InputValue(vector)
            : DecodeSurface(vector, wire, Context);
        var encoded = EncodeSurface(vector, value, context);
        if (vector.GetProperty("expect").GetString() == "accept")
            AssertWireEqual(wire, encoded, name);
    }

    private static object DecodeSurface(
        JsonElement vector,
        object wire,
        ServiceWireCodec.DecodeContext context
    )
    {
        var surface = vector.GetProperty("surface");
        if (surface.GetProperty("format").GetString() == "command")
            return surface.GetProperty("commandId").GetInt32() switch
            {
                16 => ServiceWireCodec.DecodeNodeSend16((byte[][])wire, context),
                24 => ServiceWireCodec.DecodeActorSend24((byte[][])wire, context),
                _ => throw new ConformanceHarnessException("operation command surface"),
            };
        return surface.GetProperty("format").GetString() switch
        {
            "authority-payload-v1" => ServiceWireCodec.DecodeDurableAuthorityPayloadV1(
                (byte[])wire,
                context
            ),
            "relocation-envelope-v1" => ServiceWireCodec.DecodeLogicalRelocationEnvelopeV1(
                (byte[])wire,
                context
            ),
            "type" => DecodeType(surface.GetProperty("type").GetString()!, (byte[])wire, context),
            _ => throw new ConformanceHarnessException("operation decode surface"),
        };
    }

    private static object EncodeSurface(
        JsonElement vector,
        object value,
        ServiceWireCodec.DecodeContext context
    )
    {
        var surface = vector.GetProperty("surface");
        if (surface.GetProperty("format").GetString() == "command")
            return surface.GetProperty("commandId").GetInt32() switch
            {
                16 => ServiceWireCodec.EncodeNodeSend16(
                    (ServiceWireCodec.NodeSend16)value,
                    context
                ),
                24 => ServiceWireCodec.EncodeActorSend24(
                    (ServiceWireCodec.ActorSend24)value,
                    context
                ),
                _ => throw new ConformanceHarnessException("operation command surface"),
            };
        return surface.GetProperty("format").GetString() switch
        {
            "authority-payload-v1" => ServiceWireCodec.EncodeDurableAuthorityPayloadV1(
                (ServiceWireCodec.AuthorityPayloadV1)value,
                context
            ),
            "relocation-envelope-v1" => ServiceWireCodec.EncodeLogicalRelocationEnvelopeV1(
                (ServiceWireCodec.RelocationEnvelopeV1)value,
                context
            ),
            "type" => EncodeType(surface.GetProperty("type").GetString()!, value, context),
            _ => throw new ConformanceHarnessException("operation encode surface"),
        };
    }

    private static object DecodeType(
        string type,
        byte[] bytes,
        ServiceWireCodec.DecodeContext context
    ) =>
        type switch
        {
            "application-version" => ServiceWireCodec.DecodeApplicationVersion(bytes, context),
            "bool8" => ServiceWireCodec.DecodeBool8(bytes, context),
            "rid" => ServiceWireCodec.DecodeRid(bytes, context),
            "text8" => ServiceWireCodec.DecodeText8(bytes, context),
            "optional-actor-ref" => ServiceWireCodec.DecodeOptionalActorRef(bytes, context),
            "actor-ref" => ServiceWireCodec.DecodeActorRef(bytes, context),
            "sorted-text8-vector" => ServiceWireCodec.DecodeSortedText8Vector(bytes, context),
            "metadata-frame" => ServiceWireCodec.DecodeMetadataFrame(bytes, context),
            "application-payload-envelope-v1" =>
                ServiceWireCodec.DecodeApplicationPayloadEnvelopeV1(bytes, context),
            "relocation-object-identity" => ServiceWireCodec.DecodeRelocationObjectIdentity(
                bytes,
                context
            ),
            "descriptor-extension" => ServiceWireCodec.DecodeDescriptorExtension(bytes, context),
            "aggregate-participant-vector" => ServiceWireCodec.DecodeAggregateParticipantVector(
                bytes,
                context
            ),
            "application-payload-bytes" => ServiceWireCodec.DecodeApplicationPayloadBytes(
                bytes,
                context
            ),
            "creation-operation-terminal-v1" => ServiceWireCodec.DecodeCreationOperationTerminalV1(
                bytes,
                context
            ),
            _ => throw new ConformanceHarnessException($"operation type decode {type}"),
        };

    private static byte[] EncodeType(
        string type,
        object value,
        ServiceWireCodec.DecodeContext context
    ) =>
        type switch
        {
            "application-version" => ServiceWireCodec.EncodeApplicationVersion(
                (ServiceWireCodec.ApplicationVersion)value,
                context
            ),
            "bool8" => ServiceWireCodec.EncodeBool8((ServiceWireCodec.Bool8)value, context),
            "rid" => ServiceWireCodec.EncodeRid((ServiceWireCodec.Rid)value, context),
            "text8" => ServiceWireCodec.EncodeText8((ServiceWireCodec.Text8)value, context),
            "optional-actor-ref" => ServiceWireCodec.EncodeOptionalActorRef(
                (ServiceWireCodec.OptionalActorRef)value,
                context
            ),
            "actor-ref" => ServiceWireCodec.EncodeActorRef(
                (ServiceWireCodec.ActorRef)value,
                context
            ),
            "sorted-text8-vector" => ServiceWireCodec.EncodeSortedText8Vector(
                (ServiceWireCodec.SortedText8Vector)value,
                context
            ),
            "metadata-frame" => ServiceWireCodec.EncodeMetadataFrame(
                (ServiceWireCodec.MetadataFrame)value,
                context
            ),
            "application-payload-envelope-v1" =>
                ServiceWireCodec.EncodeApplicationPayloadEnvelopeV1(
                    (ServiceWireCodec.ApplicationPayloadEnvelopeV1)value,
                    context
                ),
            "relocation-object-identity" => ServiceWireCodec.EncodeRelocationObjectIdentity(
                (ServiceWireCodec.RelocationObjectIdentity)value,
                context
            ),
            "descriptor-extension" => ServiceWireCodec.EncodeDescriptorExtension(
                (ServiceWireCodec.DescriptorExtension)value,
                context
            ),
            "aggregate-participant-vector" => ServiceWireCodec.EncodeAggregateParticipantVector(
                (ServiceWireCodec.AggregateParticipantVector)value,
                context
            ),
            "application-payload-bytes" => ServiceWireCodec.EncodeApplicationPayloadBytes(
                (ServiceWireCodec.ApplicationPayloadBytes)value,
                context
            ),
            "creation-operation-terminal-v1" => ServiceWireCodec.EncodeCreationOperationTerminalV1(
                (ServiceWireCodec.CreationOperationTerminalV1)value,
                context
            ),
            _ => throw new ConformanceHarnessException($"operation type encode {type}"),
        };

    private static object InputValue(JsonElement vector)
    {
        var name = vector.GetProperty("name").GetString()!;
        var input = vector.GetProperty("input");
        var type = vector.GetProperty("surface").GetProperty("type").GetString();
        if (name == "text-lone-surrogate")
            return new ServiceWireCodec.Text8("\ud800");
        if (type == "optional-actor-ref")
            return new ServiceWireCodec.OptionalActorRef(
                new ServiceWireCodec.OptionalText8(
                    input.GetProperty("actorId").ValueKind == JsonValueKind.Null
                        ? null
                        : input.GetProperty("actorId").GetString()
                ),
                input.GetProperty("generation").ValueKind == JsonValueKind.Null
                    ? null
                    : new ServiceWireCodec.NonzeroU64(input.GetProperty("generation").GetUInt64())
            );
        if (type == "relocation-object-identity")
            return new ServiceWireCodec.RelocationObjectIdentityCase0(
                Enum.Parse<ServiceWireCodec.StatefulObjectKind>(
                    Pascal(input.GetProperty("objectKind").GetString()!)
                ),
                new ServiceWireCodec.ActorRef(
                    new ServiceWireCodec.Text8(
                        input.GetProperty("actor").GetProperty("actorId").GetString()!
                    ),
                    new ServiceWireCodec.NonzeroU64(
                        input.GetProperty("actor").GetProperty("objectGeneration").GetUInt64()
                    )
                ),
                new ServiceWireCodec.NonzeroU64(
                    input.GetProperty("expectedAuthorityOwnerGeneration").GetUInt64()
                )
            );
        if (type == "descriptor-extension")
            return Descriptor(input);
        if (type == "aggregate-participant-vector")
            return AggregateParticipants(input);
        if (type == "sorted-text8-vector")
            return new ServiceWireCodec.SortedText8Vector(
                input
                    .EnumerateArray()
                    .Select(item => new ServiceWireCodec.Text8(item.GetString()!))
                    .ToArray()
            );
        if (type == "application-payload-bytes")
        {
            var bytes = GC.AllocateUninitializedArray<byte>(input.GetProperty("count").GetInt32());
            bytes.AsSpan().Fill((byte)input.GetProperty("repeatByte").GetInt32());
            return new ServiceWireCodec.ApplicationPayloadBytes(bytes);
        }
        if (type == "creation-operation-terminal-v1")
            return new ServiceWireCodec.CreationOperationTerminalV1(
                Enum.Parse<ServiceWireCodec.RequestTerminalResult>(
                    Pascal(input.GetProperty("terminalResult").GetString()!)
                ),
                Enum.Parse<ServiceWireCodec.FrameworkErrorCode>(
                    Pascal(input.GetProperty("failureCode").GetString()!)
                ),
                Enum.Parse<ServiceWireCodec.Bool8>(
                    Pascal(input.GetProperty("hasCreation").GetString()!)
                ),
                null,
                Enum.Parse<ServiceWireCodec.Bool8>(
                    Pascal(input.GetProperty("hasApplicationPayload").GetString()!)
                ),
                null
            );
        throw new ConformanceHarnessException($"operation input {name}");
    }

    private static ServiceWireCodec.DescriptorExtension Descriptor(JsonElement input)
    {
        T? Optional<T>(string name, Func<JsonElement, T> create)
            where T : class =>
            input.TryGetProperty(name, out var value) && value.ValueKind != JsonValueKind.Null
                ? create(value)
                : null;
        ServiceWireCodec.RuntimeState? runtimeState =
            input.TryGetProperty("runtimeState", out var runtime)
            && runtime.ValueKind != JsonValueKind.Null
                ? Enum.Parse<ServiceWireCodec.RuntimeState>(Pascal(runtime.GetString()!))
                : null;
        ServiceWireCodec.ObjectRole? objectRole =
            input.TryGetProperty("objectRole", out var role) && role.ValueKind != JsonValueKind.Null
                ? Enum.Parse<ServiceWireCodec.ObjectRole>(Pascal(role.GetString()!))
                : null;
        return new(
            runtimeState,
            Optional(
                "applicationVersion",
                value => new ServiceWireCodec.ApplicationVersion(value.GetInt64())
            ),
            null,
            null,
            null,
            Optional(
                "protocolCapabilities",
                value => new ServiceWireCodec.SortedText8Vector(
                    value
                        .EnumerateArray()
                        .Select(item => new ServiceWireCodec.Text8(item.GetString()!))
                        .ToArray()
                )
            ),
            objectRole,
            Optional("placementWeight", value => new ServiceWireCodec.U32(value.GetUInt32())),
            Optional(
                "activeCapacityLimit",
                value => new ServiceWireCodec.ObjectCapacityLimit(value.GetUInt32())
            ),
            Optional(
                "pendingCapacityLimit",
                value => new ServiceWireCodec.ObjectPendingCapacityLimit(value.GetUInt32())
            ),
            Optional("activeCapacityUsed", value => new ServiceWireCodec.U32(value.GetUInt32())),
            Optional("pendingCapacityUsed", value => new ServiceWireCodec.U32(value.GetUInt32()))
        );
    }

    private static ServiceWireCodec.AggregateParticipantVector AggregateParticipants(
        JsonElement input
    ) =>
        new(
            input
                .EnumerateArray()
                .Select(item =>
                {
                    var actor = item.GetProperty("object").GetProperty("actor");
                    var identity = new ServiceWireCodec.RelocationObjectIdentityCase0(
                        ServiceWireCodec.StatefulObjectKind.Actor,
                        new ServiceWireCodec.ActorRef(
                            new ServiceWireCodec.Text8(actor.GetProperty("actorId").GetString()!),
                            new ServiceWireCodec.NonzeroU64(
                                actor.GetProperty("objectGeneration").GetUInt64()
                            )
                        ),
                        new ServiceWireCodec.NonzeroU64(
                            item.GetProperty("object")
                                .GetProperty("expectedAuthorityOwnerGeneration")
                                .GetUInt64()
                        )
                    );
                    return new ServiceWireCodec.MaintenanceAggregateParticipantV1(
                        identity,
                        new ServiceWireCodec.AuthorityStoreVersion(
                            item.GetProperty("expectedStoreVersion").GetString()!
                        ),
                        new ServiceWireCodec.AggregateParticipantMutationBytes(
                            Convert.FromHexString(item.GetProperty("mutationHex").GetString()!)
                        )
                    );
                })
                .ToArray()
        );

    private static ServiceWireCodec.DecodeContext ContextFor(JsonElement vector)
    {
        if (!vector.TryGetProperty("context", out var values))
            return vector.GetProperty("operation").GetString() == "negotiated-bound"
                ? ServiceWireCodec.DecodeContext.Empty
                : Context;

        var payloadMaximum = values.TryGetProperty(
            "effectiveCompleteMessageBytesMinusActualEnvelopeOverhead",
            out var payload
        )
            ? payload.GetInt64()
            : (long?)null;
        long? envelopeMaximum = values.TryGetProperty(
            "effectiveCompleteMessageBytes",
            out var envelope
        )
            ? envelope.GetInt64()
            : null;
        return new(null, null, null, payloadMaximum, envelopeMaximum);
    }

    private static object OperationWire(JsonElement vector)
    {
        if (vector.TryGetProperty("framesHex", out var frames))
            return frames
                .EnumerateArray()
                .Select(item => Convert.FromHexString(item.GetString()!))
                .ToArray();
        if (vector.TryGetProperty("chunksHex", out var chunks))
            return chunks
                .EnumerateArray()
                .SelectMany(item => Convert.FromHexString(item.GetString()!))
                .ToArray();
        if (vector.TryGetProperty("byteRecipe", out var recipe))
        {
            var result = GC.AllocateUninitializedArray<byte>(
                recipe.GetProperty("encodedBytes").GetInt32()
            );
            var offset = 0;
            foreach (var segment in recipe.GetProperty("segments").EnumerateArray())
                if (segment.TryGetProperty("hex", out var hex))
                {
                    var bytes = Convert.FromHexString(hex.GetString()!);
                    bytes.CopyTo(result, offset);
                    offset += bytes.Length;
                }
                else
                {
                    var count = segment.GetProperty("count").GetInt32();
                    result
                        .AsSpan(offset, count)
                        .Fill((byte)segment.GetProperty("repeatByte").GetInt32());
                    offset += count;
                }
            Assert.Equal(result.Length, offset);
            return result;
        }
        return Convert.FromHexString(vector.GetProperty("hex").GetString()!);
    }

    private static void AssertWireEqual(object expected, object actual, string name)
    {
        if (expected is byte[] bytes && actual is byte[] encoded)
            Assert.True(bytes.SequenceEqual(encoded), name);
        else if (expected is byte[][] frames && actual is byte[][] encodedFrames)
            Assert.Equal(frames, encodedFrames);
        else
            throw new ConformanceHarnessException($"wire shape {name}");
    }

    private static byte[][] GeneratedCommandRoundTrip(int commandId, byte[][] frames) =>
        commandId switch
        {
            28 => ServiceWireCodec.EncodeActorJoin28(
                ServiceWireCodec.DecodeActorJoin28(frames, Context),
                Context
            ),
            47 => ServiceWireCodec.EncodeUserSpotCreate47(
                ServiceWireCodec.DecodeUserSpotCreate47(frames, Context),
                Context
            ),
            48 => ServiceWireCodec.EncodeUserSpotClose48(
                ServiceWireCodec.DecodeUserSpotClose48(frames, Context),
                Context
            ),
            49 => ServiceWireCodec.EncodeActorCreate49(
                ServiceWireCodec.DecodeActorCreate49(frames, Context),
                Context
            ),
            _ => throw new InvalidDataException($"unhandled command {commandId}"),
        };

    private static byte[][] PilotCommandRoundTrip(int commandId, byte[][] frames) =>
        commandId switch
        {
            28 => ServiceWirePilotCodec.EncodeActorJoin28(
                ServiceWirePilotCodec.DecodeActorJoin28(frames)
            ),
            47 =>
            [
                ServiceWirePilotCodec.EncodeUserSpotCreate47(
                    ServiceWirePilotCodec.DecodeUserSpotCreate47(frames.Single())
                ),
            ],
            48 =>
            [
                ServiceWirePilotCodec.EncodeUserSpotClose48(
                    ServiceWirePilotCodec.DecodeUserSpotClose48(frames.Single())
                ),
            ],
            49 =>
            [
                ServiceWirePilotCodec.EncodeActorCreate49(
                    ServiceWirePilotCodec.DecodeActorCreate49(frames.Single())
                ),
            ],
            _ => throw new InvalidDataException($"unhandled command oracle {commandId}"),
        };

    private static byte[] GeneratedFormatRoundTrip(string format, byte[] bytes) =>
        format switch
        {
            "authority-payload-v1" => ServiceWireCodec.EncodeDurableAuthorityPayloadV1(
                ServiceWireCodec.DecodeDurableAuthorityPayloadV1(bytes, Context),
                Context
            ),
            "instance-activation-recovery-v1" =>
                ServiceWireCodec.EncodeDurableInstanceActivationRecoveryV1(
                    ServiceWireCodec.DecodeDurableInstanceActivationRecoveryV1(bytes, Context),
                    Context
                ),
            "relocation-data-chunk-v1" => ServiceWireCodec.EncodeDurableRelocationDataChunkV1(
                ServiceWireCodec.DecodeDurableRelocationDataChunkV1(bytes, Context),
                Context
            ),
            "relocation-manifest-v1" => ServiceWireCodec.EncodeDurableRelocationManifestV1(
                ServiceWireCodec.DecodeDurableRelocationManifestV1(bytes, Context),
                Context
            ),
            "relocation-envelope-v1" => ServiceWireCodec.EncodeLogicalRelocationEnvelopeV1(
                ServiceWireCodec.DecodeLogicalRelocationEnvelopeV1(bytes, Context),
                Context
            ),
            _ => throw new InvalidDataException($"unhandled format {format}"),
        };

    private static byte[]? PilotFormatRoundTrip(string format, byte[] bytes) =>
        format switch
        {
            "relocation-data-chunk-v1" => ServiceWirePilotCodec.EncodeRelocationDataChunkV1(
                ServiceWirePilotCodec.DecodeRelocationDataChunkV1(bytes)
            ),
            "relocation-manifest-v1" => ServiceWirePilotCodec.EncodeRelocationManifestV1(
                ServiceWirePilotCodec.DecodeRelocationManifestV1(bytes)
            ),
            "relocation-envelope-v1" => ServiceWirePilotCodec.EncodeRelocationEnvelopeV1(
                ServiceWirePilotCodec.DecodeRelocationEnvelopeV1(bytes)
            ),
            _ => null,
        };

    private static byte[][] Frames(JsonElement fixture, JsonElement vector)
    {
        var pointers = vector.GetProperty("pointers");
        var pointer = pointers.TryGetProperty("framesHex", out var framesHex)
            ? framesHex
            : pointers.GetProperty("hex");
        var value = Pointer(fixture, pointer);
        return value.ValueKind == JsonValueKind.Array
            ? value
                .EnumerateArray()
                .Select(item => Convert.FromHexString(item.GetString()!))
                .ToArray()
            : [Convert.FromHexString(value.GetString()!)];
    }

    private static JsonElement Pointer(JsonElement root, JsonElement pointer)
    {
        var current = root;
        foreach (var segment in pointer.GetString()!.Split('/').Skip(1))
        {
            var decoded = segment.Replace("~1", "/").Replace("~0", "~");
            current =
                current.ValueKind == JsonValueKind.Array
                    ? current[int.Parse(decoded)]
                    : current.GetProperty(decoded);
        }
        return current;
    }

    private static string Pascal(string value) => char.ToUpperInvariant(value[0]) + value[1..];

    private static JsonDocument ReadJson(string relativePath)
    {
        var frameworkRoot = Common.FrameworkTestEnvironment.GetFrameworkRoot();
        var path = Path.GetFullPath($"../../runtime/protocol/{relativePath}", frameworkRoot);
        return JsonDocument.Parse(File.ReadAllText(path));
    }

    private sealed record ConformanceCase(string Label, string Expect, Action Exercise);

    private sealed class ConformanceHarnessException(string message) : Exception(message);
}

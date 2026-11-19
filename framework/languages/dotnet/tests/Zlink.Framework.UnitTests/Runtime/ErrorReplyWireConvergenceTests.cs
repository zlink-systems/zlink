using System.Text.Json;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

/// <summary>
/// Cross-language error reply wire convergence: snake_case errorCode names,
/// the envelope metadata field, the zlink.origin=framework marker, and the
/// origin-narrowed stale-route judgement. Canonical peers: C++
/// channel_reply_writer.cpp (errorCode table), failure_origin_wire.hpp
/// (marker), channel_runtime.cpp (stale-route rule).
/// </summary>
public sealed class ErrorReplyWireConvergenceTests
{
    //  Pinned 1:1 against C++ channel_reply_writer.cpp error_code_name().
    private static readonly (ZLinkFrameworkErrorKind Kind, string Name)[] WireNameTable =
    [
        (ZLinkFrameworkErrorKind.NotFound, "not_found"),
        (ZLinkFrameworkErrorKind.AlreadyExists, "already_exists"),
        (ZLinkFrameworkErrorKind.TypeMismatch, "type_mismatch"),
        (ZLinkFrameworkErrorKind.NotConfigured, "not_configured"),
        (ZLinkFrameworkErrorKind.Rejected, "rejected"),
        (ZLinkFrameworkErrorKind.Unavailable, "unavailable"),
        (ZLinkFrameworkErrorKind.DeadlineExceeded, "deadline_exceeded"),
        (ZLinkFrameworkErrorKind.ShuttingDown, "shutting_down"),
        (ZLinkFrameworkErrorKind.ProtocolError, "protocol_error"),
        (ZLinkFrameworkErrorKind.InvalidOperation, "invalid_operation"),
        (ZLinkFrameworkErrorKind.DataLost, "data_lost"),
        (ZLinkFrameworkErrorKind.InternalFailure, "internal_failure"),
    ];

    [Fact]
    public void ErrorCode_wire_names_pin_the_canonical_cpp_table()
    {
        //  A new enum member must extend the cross-language table explicitly.
        Assert.Equal(12, Enum.GetValues<ZLinkFrameworkErrorKind>().Length);
        Assert.Equal(12, WireNameTable.Length);

        foreach (var (kind, name) in WireNameTable)
        {
            Assert.Equal(name, ZLinkErrorWireNames.Name(kind));
            Assert.True(ZLinkErrorWireNames.TryParse(name, out var parsed));
            Assert.Equal(kind, parsed);
        }
    }

    [Theory]
    [InlineData("NotFound")]
    [InlineData("Not_Found")]
    [InlineData("NOT_FOUND")]
    [InlineData("0")]
    [InlineData("")]
    [InlineData(null)]
    public void ErrorCode_parsing_accepts_snake_case_only(string? code)
    {
        Assert.False(ZLinkErrorWireNames.TryParse(code, out _));
    }

    [Fact]
    public void Envelope_header_metadata_roundtrips()
    {
        var header = new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Command,
            "play",
            "Move",
            ZLinkEnvelopeCodec.DefaultContentType,
            null,
            null,
            null,
            null,
            null
        )
        {
            Metadata = new Dictionary<string, string>
            {
                ["zlink.origin"] = "framework",
                ["custom"] = "value",
            },
        };

        using var encoded = ZLinkEnvelopeCodec.EncodeHeader(header);
        var decoded = ZLinkEnvelopeCodec.DecodeHeader(encoded);

        Assert.NotNull(decoded.Metadata);
        Assert.Equal("framework", decoded.Metadata!["zlink.origin"]);
        Assert.Equal("value", decoded.Metadata["custom"]);
    }

    [Fact]
    public void Envelope_header_without_metadata_omits_the_wire_field()
    {
        var header = new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Command,
            "play",
            "Move",
            ZLinkEnvelopeCodec.DefaultContentType,
            null,
            null,
            null,
            null,
            null
        );

        using var encoded = ZLinkEnvelopeCodec.EncodeHeader(header);
        Assert.Null(ZLinkEnvelopeCodec.DecodeHeader(encoded).Metadata);
    }

    private const string FrameworkErrorHeaderHex =
        "08051204706c61791a044d6f766522106170706c69636174696f6e2f6a736f6e2a06636f72722d313a096e6f745f666f756e64421853706f7420726f75746520776173206e6f7420666f756e646a190a0c7a6c696e6b2e6f726967696e12096672616d65776f726b";
    private const string ApplicationErrorHeaderHex =
        "08051204706c61791a044d6f766522106170706c69636174696f6e2f6a736f6e2a06636f72722d313a096e6f745f666f756e6442136f7264657220776173206e6f7420666f756e64";

    [Fact]
    public void Cpp_golden_framework_error_header_decodes_to_a_stale_route_signal()
    {
        using var message = Message.From(Convert.FromHexString(FrameworkErrorHeaderHex));
        var header = ZLinkEnvelopeCodec.DecodeHeader(message);

        Assert.Equal(ZLinkMessageKind.Error, header.Kind);
        Assert.Equal("not_found", header.ErrorCode);
        Assert.Equal("Spot route was not found", header.ErrorMessage);
        Assert.NotNull(header.Metadata);
        Assert.Equal("framework", header.Metadata!["zlink.origin"]);

        var error = Assert.IsType<ZLinkFrameworkException>(
            ZLinkEnvelopeErrorMapper.CreateException(header, "fallback")
        );
        Assert.Equal(ZLinkFrameworkErrorKind.NotFound, error.Kind);
        Assert.Equal(ZLinkErrorOrigin.Framework, error.Origin);
        Assert.True(ZLinkSpotHandleRequestExecution.IsStaleRoute(error));
    }

    [Fact]
    public void Cpp_golden_application_error_header_is_not_a_stale_route_signal()
    {
        using var message = Message.From(Convert.FromHexString(ApplicationErrorHeaderHex));
        var header = ZLinkEnvelopeCodec.DecodeHeader(message);

        Assert.Equal("not_found", header.ErrorCode);
        Assert.Null(header.Metadata);

        var error = Assert.IsType<ZLinkFrameworkException>(
            ZLinkEnvelopeErrorMapper.CreateException(header, "fallback")
        );
        Assert.Equal(ZLinkFrameworkErrorKind.NotFound, error.Kind);
        Assert.Equal(ZLinkErrorOrigin.Application, error.Origin);
        Assert.False(ZLinkSpotHandleRequestExecution.IsStaleRoute(error));
    }

    [Fact]
    public void Framework_generated_spot_error_reply_carries_the_origin_marker()
    {
        var parts = ZLinkSpotReplyEnvelope.EncodeErrorParts(
            "play",
            "Move",
            "corr-1",
            new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.NotFound,
                "No SPOT route request handler is registered."
            )
            {
                Origin = ZLinkErrorOrigin.Framework,
            }
        );
        try
        {
            var header = ZLinkEnvelopeCodec.DecodeHeader(parts);
            Assert.Equal(ZLinkMessageKind.Error, header.Kind);
            Assert.Equal("not_found", header.ErrorCode);
            Assert.Equal("framework", header.Metadata!["zlink.origin"]);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
    }

    [Fact]
    public void Application_handler_error_reply_stays_unmarked()
    {
        var parts = ZLinkSpotReplyEnvelope.EncodeErrorParts(
            "play",
            "Move",
            "corr-1",
            new ZLinkFrameworkException(ZLinkFrameworkErrorKind.NotFound, "order was not found")
        );
        try
        {
            var header = ZLinkEnvelopeCodec.DecodeHeader(parts);
            Assert.Null(header.Metadata);
            var error = Assert.IsType<ZLinkFrameworkException>(
                ZLinkEnvelopeErrorMapper.CreateException(header, "fallback")
            );
            Assert.Equal(ZLinkFrameworkErrorKind.NotFound, error.Kind);
            Assert.Equal(ZLinkErrorOrigin.Application, error.Origin);
            Assert.False(ZLinkSpotHandleRequestExecution.IsStaleRoute(error));
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
    }

    [Fact]
    public void Forced_framework_origin_marks_internal_route_error_replies()
    {
        var parts = ZLinkSpotReplyEnvelope.EncodeErrorParts(
            "play",
            "$zlink.actor-join",
            "corr-1",
            new InvalidOperationException("join failed"),
            forceFrameworkOrigin: true
        );
        try
        {
            var header = ZLinkEnvelopeCodec.DecodeHeader(parts);
            Assert.Equal("framework", header.Metadata!["zlink.origin"]);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
    }

    [Fact]
    public void Protocol_error_replies_are_framework_marked()
    {
        var request = new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Request,
            "play",
            "Move",
            ZLinkEnvelopeCodec.DefaultContentType,
            "corr-1",
            null,
            null,
            null,
            null
        );
        var parts = ZLinkSpotReplyEnvelope.EncodeProtocolErrorParts(
            "play",
            request,
            "malformed frame"
        );
        try
        {
            var header = ZLinkEnvelopeCodec.DecodeHeader(parts);
            Assert.Equal("protocol_error", header.ErrorCode);
            Assert.True(
                header.Metadata is not null && header.Metadata["zlink.origin"] == "framework"
            );
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }

        var channelHeader = ZLinkChannelReplyWriter.CreateProtocolErrorHeader(
            "play",
            request,
            "malformed frame"
        );
        Assert.Equal("protocol_error", channelHeader.ErrorCode);
        Assert.True(
            channelHeader.Metadata is not null
                && channelHeader.Metadata["zlink.origin"] == "framework"
        );
    }

    [Fact]
    public void Channel_error_header_roundtrips_snake_case_and_marker()
    {
        var request = new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Request,
            "route",
            "Lookup",
            ZLinkEnvelopeCodec.DefaultContentType,
            "corr-1",
            null,
            null,
            null,
            null
        );
        var replyHeader = ZLinkChannelReplyWriter.CreateErrorHeader(
            "route",
            request,
            new ZLinkFrameworkException(ZLinkFrameworkErrorKind.Unavailable, "route owner is gone")
            {
                Origin = ZLinkErrorOrigin.Framework,
            }
        );

        Assert.Equal("unavailable", replyHeader.ErrorCode);

        using var encoded = ZLinkEnvelopeCodec.EncodeHeader(replyHeader);
        var decoded = ZLinkEnvelopeCodec.DecodeHeader(encoded);
        var error = Assert.IsType<ZLinkFrameworkException>(
            ZLinkEnvelopeErrorMapper.CreateException(decoded, "fallback")
        );

        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, error.Kind);
        Assert.Equal(ZLinkErrorOrigin.Framework, error.Origin);
        Assert.True(ZLinkSpotHandleRequestExecution.IsStaleRoute(error));
    }

    [Fact]
    public void All_13_error_kinds_roundtrip_through_the_error_envelope()
    {
        foreach (var (kind, name) in WireNameTable)
        {
            var parts = ZLinkSpotReplyEnvelope.EncodeErrorParts(
                "play",
                "Move",
                "corr-1",
                new ZLinkFrameworkException(kind, $"failed with {kind}")
            );
            try
            {
                var header = ZLinkEnvelopeCodec.DecodeHeader(parts);
                Assert.Equal(name, header.ErrorCode);

                var error = Assert.IsType<ZLinkFrameworkException>(
                    ZLinkEnvelopeErrorMapper.CreateException(header, "fallback")
                );
                Assert.Equal(kind, error.Kind);
                Assert.Equal($"failed with {kind}", error.Message);
            }
            finally
            {
                ZLinkMessageParts.DisposeAll(parts);
            }
        }
    }

    //  Origin values: 0 Unspecified (local failure), 1 Framework, 2 Application
    //  (the internal enum cannot appear in a public theory signature).
    [Theory]
    [InlineData(ZLinkFrameworkErrorKind.NotFound, 1, true)]
    [InlineData(ZLinkFrameworkErrorKind.Unavailable, 1, true)]
    [InlineData(ZLinkFrameworkErrorKind.NotFound, 2, false)]
    [InlineData(ZLinkFrameworkErrorKind.Unavailable, 2, false)]
    [InlineData(ZLinkFrameworkErrorKind.NotFound, 0, true)]
    [InlineData(ZLinkFrameworkErrorKind.Unavailable, 0, true)]
    [InlineData(ZLinkFrameworkErrorKind.Rejected, 1, false)]
    [InlineData(ZLinkFrameworkErrorKind.InternalFailure, 0, false)]
    public void Stale_route_judgement_requires_a_non_application_origin(
        ZLinkFrameworkErrorKind kind,
        int origin,
        bool expected
    )
    {
        var error = new ZLinkFrameworkException(kind, "failed")
        {
            Origin = (ZLinkErrorOrigin)origin,
        };

        Assert.Equal(expected, ZLinkSpotHandleRequestExecution.IsStaleRoute(error));
    }

    [Fact]
    public void Stream_wire_error_uses_the_snake_case_table()
    {
        foreach (var (kind, name) in WireNameTable)
        {
            var wire = ZLinkStreamWireError.FromException(
                new ZLinkFrameworkException(kind, "failed")
            );
            Assert.Equal(name, wire.Code);
        }
    }

    [Fact]
    public void Stream_wire_error_maps_non_framework_exceptions_to_internal_failure()
    {
        var wire = ZLinkStreamWireError.FromException(new InvalidOperationException("failed"));
        Assert.Equal("internal_failure", wire.Code);
        var parts = ActorErrorReplyParts(
            JsonSerializer.Serialize(wire, ZLinkJsonSerializerOptions.Default)
        );
        try
        {
            var error = Assert.Throws<ZLinkFrameworkException>(() =>
                ZLinkActorReplyDecoder.Decode<object>(parts)
            );
            Assert.Equal(ZLinkFrameworkErrorKind.InternalFailure, error.Kind);
            Assert.Equal("failed", error.Message);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
    }

    [Fact]
    public void Actor_reply_decoder_rejects_clr_exception_names()
    {
        var parts = ActorErrorReplyParts(
            "{\"code\":\"InvalidOperationException\",\"message\":\"failed\"}"
        );
        try
        {
            var error = Assert.Throws<ZLinkFrameworkException>(() =>
                ZLinkActorReplyDecoder.Decode<object>(parts)
            );
            Assert.Equal(ZLinkFrameworkErrorKind.ProtocolError, error.Kind);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
    }

    [Fact]
    public void Actor_reply_decoder_maps_snake_case_stream_wire_codes()
    {
        foreach (var (kind, name) in WireNameTable)
        {
            var parts = ActorErrorReplyParts(
                $"{{\"code\":\"{name}\",\"message\":\"failed with {name}\"}}"
            );
            try
            {
                var error = Assert.Throws<ZLinkFrameworkException>(() =>
                    ZLinkActorReplyDecoder.Decode<object>(parts)
                );
                Assert.Equal(kind, error.Kind);
                Assert.Equal($"failed with {name}", error.Message);
            }
            finally
            {
                ZLinkMessageParts.DisposeAll(parts);
            }
        }
    }

    private static IReadOnlyList<Message> ActorErrorReplyParts(string payload)
    {
        var header = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Error,
            ZlinkStreamCodec.Json,
            ZlinkStreamHeaderFlags.HasRequestSeq,
            new ZlinkStreamRequestSeq(1),
            string.Empty,
            ZlinkStreamMetadata.Empty
        );
        return
        [
            Message.From(
                ZLinkStreamFrameCodec.Encode(
                    ZLinkStreamProtocolDefaults.EncodeHeader(header).Span,
                    System.Text.Encoding.UTF8.GetBytes(payload)
                )
            ),
        ];
    }
}

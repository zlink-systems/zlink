using System.Net;
using System.Text.Encodings.Web;
using System.Text.Json;
using Zlink.Framework.Contracts.Errors;

namespace Tutorial.Client;

// --8<-- [start:error-mapping]
// A framework call fails by throwing ZLinkFrameworkException. Left alone, it
// reaches the host's default handler and every failure looks like a 500 — the
// caller cannot tell "no node is available right now" from "this server has a
// bug". This middleware turns the error kind into the status code that says
// what actually happened.
public static class ZLinkErrorResponse
{
    // Keeps apostrophes in framework messages readable instead of '.
    private static readonly JsonSerializerOptions BodyFormat = new()
    {
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    // The same table the framework's own HTTP host uses.
    private static HttpStatusCode StatusFor(ZLinkFrameworkErrorKind kind) =>
        kind switch
        {
            ZLinkFrameworkErrorKind.ProtocolError
            or ZLinkFrameworkErrorKind.TypeMismatch
            or ZLinkFrameworkErrorKind.InvalidOperation => HttpStatusCode.BadRequest,
            ZLinkFrameworkErrorKind.NotFound => HttpStatusCode.NotFound,
            ZLinkFrameworkErrorKind.AlreadyExists => HttpStatusCode.Conflict,
            ZLinkFrameworkErrorKind.Rejected => HttpStatusCode.Forbidden,
            // No node can take the call now. The caller may retry.
            ZLinkFrameworkErrorKind.NotConfigured
            or ZLinkFrameworkErrorKind.Unavailable
            or ZLinkFrameworkErrorKind.ShuttingDown => HttpStatusCode.ServiceUnavailable,
            ZLinkFrameworkErrorKind.DeadlineExceeded => HttpStatusCode.GatewayTimeout,
            _ => HttpStatusCode.InternalServerError,
        };

    private static string NameFor(ZLinkFrameworkErrorKind kind) =>
        kind switch
        {
            ZLinkFrameworkErrorKind.NotFound => "not_found",
            ZLinkFrameworkErrorKind.AlreadyExists => "already_exists",
            ZLinkFrameworkErrorKind.TypeMismatch => "type_mismatch",
            ZLinkFrameworkErrorKind.NotConfigured => "not_configured",
            ZLinkFrameworkErrorKind.Rejected => "rejected",
            ZLinkFrameworkErrorKind.Unavailable => "unavailable",
            ZLinkFrameworkErrorKind.DeadlineExceeded => "deadline_exceeded",
            ZLinkFrameworkErrorKind.ShuttingDown => "shutting_down",
            ZLinkFrameworkErrorKind.ProtocolError => "protocol_error",
            ZLinkFrameworkErrorKind.InvalidOperation => "invalid_operation",
            ZLinkFrameworkErrorKind.DataLost => "data_lost",
            _ => "internal_failure",
        };

    // Register before the endpoints so it wraps every call below it.
    public static IApplicationBuilder UseZLinkErrorResponse(this IApplicationBuilder app) =>
        app.Use(
            async (context, next) =>
            {
                try
                {
                    await next();
                }
                catch (ZLinkFrameworkException error)
                {
                    context.Response.StatusCode = (int)StatusFor(error.Kind);
                    context.Response.ContentType = "application/json";
                    await context.Response.WriteAsync(
                        JsonSerializer.Serialize(
                            new { error = NameFor(error.Kind), message = error.Message },
                            BodyFormat
                        )
                    );
                }
            }
        );
}
// --8<-- [end:error-mapping]

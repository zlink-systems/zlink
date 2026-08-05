using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;
using Zlink.Framework.Contracts.Dispatch;

namespace ObservabilityOps.Server.Support;

public static class DiagnosticsEndpointExtensions
{
    public static WebApplication MapDiagnosticsControl(this WebApplication app)
    {
        app.MapPost("/diagnostics/level", (
            string level,
            IZLinkDiagnosticsRuntime diagnostics) =>
        {
            diagnostics.Level = level switch
            {
                "off" => ZLinkDiagnosticsLevel.Off,
                "errors_only" => ZLinkDiagnosticsLevel.Errors,
                "key_transitions" => ZLinkDiagnosticsLevel.Normal,
                "detailed" => ZLinkDiagnosticsLevel.Detailed,
                _ => throw new ArgumentOutOfRangeException(nameof(level))
            };
            return Results.Ok(new
            {
                level,
                applied = diagnostics.Level.ToString()
            });
        });
        return app;
    }
}

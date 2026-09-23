using QuickStart.Shared;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Channels;

var builder = WebApplication.CreateBuilder(args);

// http://127.0.0.1:5080/hello/{name} is this process's own HTTP surface.
builder.WebHost.UseUrls("http://127.0.0.1:5080");

builder.Services.AddZLinkFramework(options =>
{
    // This process also needs its own endpoint.
    var mesh = options.AddRouteMesh("services").Listen("tcp://127.0.0.1:7102");
    // This side only calls; it does not handle "greeting".
    mesh.Channel("greeting").Client();
    // Manual connection — the server's endpoint is given directly.
    mesh.PeerConnections.Connect("tcp://127.0.0.1:7101");
});

var app = builder.Build();

app.MapGet(
    "/hello/{name}",
    async (string name, IZLinkRouteClient route, CancellationToken cancellationToken) =>
    {
        // The target is a single ChannelName; which node handles it is not specified.
        var reply = await route
            .RequestToChannel("greeting", new Hello(name))
            .Async<Greeting>(cancellationToken);

        return Results.Ok(reply.Text);
    }
);

await app.RunAsync();

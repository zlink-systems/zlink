using QuickStart.Shared;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Handlers;

var builder = WebApplication.CreateBuilder(args);

// This process does not expose HTTP; the port only needs to differ from the
// client process's port when both run on the same host.
builder.WebHost.UseUrls("http://127.0.0.1:5081");

builder.Services.AddZLinkFramework(options =>
{
    // Names the mesh and opens this process's endpoint for peers to connect to.
    var mesh = options.AddRouteMesh("services")
        .Listen("tcp://0.0.0.0:7101");
    // This process handles the "greeting" channel. AddHandlersFromAssemblyOf
    // only auto-wires attribute-declared handlers; an IZLinkRequestHandler<,>
    // class registers on the channel builder explicitly, as the same guide's
    // ClientServer channel example (chapter 8) does.
    mesh.Channel("greeting").Server()
        .AddRequestHandler<HelloHandler, Hello, Greeting>();
});

var app = builder.Build();
await app.RunAsync();

// Handles one request on the "greeting" channel.
public sealed class HelloHandler : IZLinkRequestHandler<Hello, Greeting>
{
    public ValueTask<Greeting> HandleAsync(
        Hello request,
        IZLinkMessageContext context,
        CancellationToken cancellationToken)
        => ValueTask.FromResult(new Greeting($"hello, {request.Name}"));
}

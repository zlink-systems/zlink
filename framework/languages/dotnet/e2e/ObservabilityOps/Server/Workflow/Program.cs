using ObservabilityOps.Server.Workflow;

var app = WorkflowHostFactory.Create(args);
await app.RunAsync();

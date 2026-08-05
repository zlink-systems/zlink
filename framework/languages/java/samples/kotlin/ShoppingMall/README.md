# ShoppingMall Kotlin

Kotlin version of the shopping mall order workflow sample.

`CommerceApi` receives order requests and sends workflow messages over ZLink.
`OrderWorkflow` advances each order through the expected state changes and
supports failure, consistency, rebuild, and scale-out checks. The client is a
self-checking scenario that validates the visible order results.

## Run

Run the complete sample scenario on Linux or WSL:

```bash
./run_sample.sh
```

On Windows:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\run_sample.ps1
```

## Layout

- `Shared/` contains order request, response, and state contracts.
- `Client/` contains the self-checking order workflow scenario.
- `Server/CommerceApi/` receives order requests and queries order results.
- `Server/OrderWorkflow/` owns order state transitions and projection rebuild.
- `Server/Configuration/` contains endpoint, channel, location-store, and packet settings.

The successful run prints `shoppingmall=completed` from the client and
`shoppingmall-server-evidence=completed` after server evidence is checked.

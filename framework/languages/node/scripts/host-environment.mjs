// Host environment pass-through for sample tooling that spawns third-party
// processes (vite preview, Playwright). This is not sample configuration:
// every sample role is configured only through --config <path>, and the
// samples/<Sample> sources themselves never read process.env (enforced by
// test/contract/sample-zoneworld-gate.test).
export function hostEnvironment(overrides = {}) {
  return { ...process.env, ...overrides };
}

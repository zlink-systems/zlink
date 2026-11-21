import path from 'node:path';

/** The retired encoder exists only in a fixture; runtime retains its readers. */
export const LegacyRelocationAuthorityFixture: typeof import('../contract/fixtures/legacy-relocation-authority').LegacyRelocationAuthorityFixture =
  require(
    path.resolve(process.cwd(), 'test/contract/fixtures/legacy-relocation-authority')
  ).LegacyRelocationAuthorityFixture;

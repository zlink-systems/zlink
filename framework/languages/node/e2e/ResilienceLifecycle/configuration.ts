import fs from 'node:fs';
import { Module, type DynamicModule } from '@nestjs/common';
import { ConfigModule, ConfigService } from '@nestjs/config';

const RESILIENCE_OPTIONS = Symbol.for('@zlink-systems/e2e-resilience:options');

function createResilienceConfigurationModule<T>(validate: (value: unknown) => T): DynamicModule {
  const args = process.argv.slice(2);
  if (args.length !== 2 || args[0] !== '--config' || args[1].startsWith('--')) throw new Error('--config <path> is the only supported framework host argument.');
  const configPath = args[1];
  class ResilienceConfigurationModule {}
  Module({})(ResilienceConfigurationModule);
  return {
    module: ResilienceConfigurationModule,
    imports: [ConfigModule.forRoot({ cache: true, ignoreEnvFile: true, isGlobal: false, load: [() => ({ e2e: readConfig(configPath) })], skipProcessEnv: true, validatePredefined: false })],
    providers: [{ provide: RESILIENCE_OPTIONS, inject: [ConfigService], useFactory: (config: ConfigService) => validate(config.get('e2e')) }],
    exports: [RESILIENCE_OPTIONS]
  };
}

function readConfig(path: string): unknown {
  const document = JSON.parse(fs.readFileSync(path, 'utf8')) as { e2e?: unknown };
  if (document.e2e === undefined) throw new Error("Configuration section 'e2e' is required.");
  return document.e2e;
}

function objectValues(value: unknown): Record<string, unknown> {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) throw new Error("Configuration section 'e2e' must be an object.");
  return value as Record<string, unknown>;
}

function optionalString(values: Record<string, unknown>, key: string): string | undefined {
  const value = values[key];
  return typeof value === 'string' && value.length > 0 ? value : undefined;
}

export { RESILIENCE_OPTIONS, createResilienceConfigurationModule, objectValues, optionalString };

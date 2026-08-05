import fs from 'node:fs';
import { Module, type DynamicModule, type InjectionToken } from '@nestjs/common';
import { ConfigModule, ConfigService } from '@nestjs/config';

export function createSpotServiceConfigurationModule<T>(
  token: InjectionToken,
  validate: (value: unknown) => T
): DynamicModule {
  const configPath = readConfigPath(process.argv.slice(2));
  class SpotServiceConfigurationModule {}
  Module({})(SpotServiceConfigurationModule);
  return {
    module: SpotServiceConfigurationModule,
    imports: [ConfigModule.forRoot({
      cache: true,
      ignoreEnvFile: true,
      isGlobal: false,
      load: [() => ({ e2e: readConfiguration(configPath) })],
      skipProcessEnv: true,
      validatePredefined: false
    })],
    providers: [{
      provide: token,
      inject: [ConfigService],
      useFactory: (config: ConfigService) => validate(config.get('e2e'))
    }],
    exports: [token]
  };
}

export function objectValues(value: unknown): Record<string, unknown> {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    throw new Error("Configuration section 'e2e' must be an object.");
  }
  return value as Record<string, unknown>;
}

export function requiredString(values: Record<string, unknown>, key: string): string {
  const value = values[key];
  if (typeof value !== 'string' || value.length === 0) {
    throw new Error(`Configuration value 'e2e.${key}' must be a non-empty string.`);
  }
  return value;
}

export function optionalString(values: Record<string, unknown>, key: string): string | undefined {
  const value = values[key];
  if (value === undefined) return undefined;
  if (typeof value !== 'string' || value.length === 0) {
    throw new Error(`Configuration value 'e2e.${key}' must be a non-empty string when provided.`);
  }
  return value;
}

export function stringList(values: Record<string, unknown>, key: string): readonly string[] {
  const value = values[key];
  if (value === undefined) return [];
  if (!Array.isArray(value) || value.some((entry) => typeof entry !== 'string' || entry.length === 0)) {
    throw new Error(`Configuration value 'e2e.${key}' must be an array of non-empty strings.`);
  }
  return value as string[];
}

export function optionalBoolean(values: Record<string, unknown>, key: string, fallback = false): boolean {
  const value = values[key];
  if (value === undefined) return fallback;
  if (typeof value !== 'boolean') {
    throw new Error(`Configuration value 'e2e.${key}' must be a boolean.`);
  }
  return value;
}

function readConfigPath(args: readonly string[]): string {
  if (args.length !== 2 || args[0] !== '--config' || args[1].startsWith('--')) {
    throw new Error('--config <path> is the only supported framework host argument.');
  }
  return args[1];
}

function readConfiguration(path: string): unknown {
  const document = JSON.parse(fs.readFileSync(path, 'utf8')) as { e2e?: unknown };
  if (document.e2e === undefined) {
    throw new Error("Configuration section 'e2e' is required.");
  }
  return document.e2e;
}

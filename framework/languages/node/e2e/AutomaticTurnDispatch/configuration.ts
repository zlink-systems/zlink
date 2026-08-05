import * as fs from 'node:fs';
import { Module, type DynamicModule, type InjectionToken } from '@nestjs/common';
import { ConfigModule, ConfigService } from '@nestjs/config';

function createAutomaticTurnConfigurationModule<T>(
  token: InjectionToken,
  validate: (value: unknown) => T
): DynamicModule {
  const options = validate(readConfiguration(readConfigPath(process.argv.slice(2))));
  return automaticTurnConfigurationModule(token, options);
}

function createAutomaticTurnConfiguration<T>(
  token: InjectionToken,
  validate: (value: unknown) => T
): { readonly module: DynamicModule; readonly options: T } {
  const options = validate(readConfiguration(readConfigPath(process.argv.slice(2))));
  return { module: automaticTurnConfigurationModule(token, options), options };
}

function automaticTurnConfigurationModule<T>(token: InjectionToken, options: T): DynamicModule {
  class AutomaticTurnConfigurationModule {}
  Module({})(AutomaticTurnConfigurationModule);
  return {
    module: AutomaticTurnConfigurationModule,
    imports: [ConfigModule.forRoot({
      cache: true,
      ignoreEnvFile: true,
      isGlobal: false,
      load: [() => ({ e2e: options })],
      skipProcessEnv: true,
      validatePredefined: false
    })],
    providers: [{
      provide: token,
      inject: [ConfigService],
      useFactory: (config: ConfigService) => {
        const configured = config.get<T>('e2e');
        if (configured === undefined) throw new Error("Configuration section 'e2e' is required.");
        return configured;
      }
    }],
    exports: [token]
  };
}

function readConfigPath(args: readonly string[]): string {
  if (args.length !== 2 || args[0] !== '--config' || args[1].startsWith('--')) {
    throw new Error('--config <path> is the only supported framework host argument.');
  }
  return args[1];
}

function readConfiguration(configPath: string): unknown {
  const document = JSON.parse(fs.readFileSync(configPath, 'utf8')) as { e2e?: unknown };
  if (document.e2e === undefined) throw new Error("Configuration section 'e2e' is required.");
  return document.e2e;
}

function objectValue(value: unknown): Record<string, unknown> {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    throw new Error("Configuration section 'e2e' must be an object.");
  }
  return value as Record<string, unknown>;
}

function requiredString(values: Record<string, unknown>, key: string): string {
  const value = values[key];
  if (typeof value !== 'string' || value.length === 0) {
    throw new Error(`Configuration value 'e2e.${key}' must be a non-empty string.`);
  }
  return value;
}

function optionalString(values: Record<string, unknown>, key: string): string | undefined {
  const value = values[key];
  if (value === undefined) return undefined;
  if (typeof value !== 'string' || value.length === 0) {
    throw new Error(`Configuration value 'e2e.${key}' must be a non-empty string when present.`);
  }
  return value;
}

function stringList(values: Record<string, unknown>, key: string, required: boolean): readonly string[] {
  const value = values[key];
  if (value === undefined && !required) return [];
  if (!Array.isArray(value) || value.some((entry) => typeof entry !== 'string' || entry.length === 0)) {
    throw new Error(`Configuration value 'e2e.${key}' must be an array of non-empty strings.`);
  }
  if (required && value.length === 0) throw new Error(`Configuration value 'e2e.${key}' must not be empty.`);
  return value as string[];
}

function peerList(values: Record<string, unknown>, key: string): readonly { rid: string; endpoint: string }[] {
  const value = values[key];
  if (value === undefined) return [];
  if (!Array.isArray(value)) throw new Error(`Configuration value 'e2e.${key}' must be an array.`);
  return value.map((entry) => {
    const peer = objectValue(entry);
    return { rid: requiredString(peer, 'rid'), endpoint: requiredString(peer, 'endpoint') };
  });
}

export {
  createAutomaticTurnConfiguration,
  createAutomaticTurnConfigurationModule,
  objectValue,
  optionalString,
  peerList,
  requiredString,
  stringList
};

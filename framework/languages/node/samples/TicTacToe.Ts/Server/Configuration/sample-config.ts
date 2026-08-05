import * as fs from 'node:fs';
import { Module, type DynamicModule } from '@nestjs/common';
import { ConfigModule, ConfigService } from '@nestjs/config';

type TicTacToeSampleConfig = {
  instanceName: string;
  apiIndex: number;
  playIndex: number;
  apiHttpEndpoint: string;
  apiEndpoints: string[];
  apiHttpEndpoints: string[];
  playEndpoints: string[];
  playSpotEndpoint: string;
  playSpotEndpoints: string[];
  playStreamEndpoint: string;
  redisEndpoint: string;
  redisKeyPrefix: string;
  peerPlaySpotEndpoint: string;
  logDir: string;
};

const TICTACTOE_SAMPLE_CONFIG = Symbol.for('TICTACTOE_SAMPLE_CONFIG');
class TicTacToeConfigurationModule {}
Module({})(TicTacToeConfigurationModule);

function createTicTacToeConfigurationModule(
  requiredKeys: readonly (keyof TicTacToeSampleConfig)[]
): DynamicModule {
  const configPath = readConfigPath(process.argv.slice(2));
  return {
    module: TicTacToeConfigurationModule,
    imports: [ConfigModule.forRoot({
      cache: true,
      ignoreEnvFile: true,
      isGlobal: false,
      load: [() => ({ sample: readSampleConfig(configPath) })],
      skipProcessEnv: true,
      validatePredefined: false
    })],
    providers: [{
      provide: TICTACTOE_SAMPLE_CONFIG,
      inject: [ConfigService],
      useFactory: (config: ConfigService) => validateSampleConfig(config.get('sample'), requiredKeys)
    }],
    exports: [TICTACTOE_SAMPLE_CONFIG]
  };
}

function readConfigPath(args: readonly string[]): string {
  if (args.length !== 2 || args[0] !== '--config' || args[1].startsWith('--')) {
    throw new Error('--config <path> is the only supported framework host argument.');
  }
  return args[1];
}

function readSampleConfig(configPath: string): unknown {
  const document = JSON.parse(fs.readFileSync(configPath, 'utf8')) as { sample?: unknown };
  if (document.sample === undefined) throw new Error("Configuration section 'sample' is required.");
  return document.sample;
}

function validateSampleConfig(
  value: unknown,
  requiredKeys: readonly (keyof TicTacToeSampleConfig)[]
): TicTacToeSampleConfig {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    throw new Error("Configuration section 'sample' must be an object.");
  }
  const config = value as Record<string, unknown>;
  for (const key of requiredKeys) validateValue(key, config[key]);
  return config as TicTacToeSampleConfig;
}

function validateValue(key: keyof TicTacToeSampleConfig, value: unknown): void {
  if (typeof value === 'string' && value.length > 0) return;
  if (typeof value === 'number' && Number.isInteger(value) && value >= 0) return;
  if (Array.isArray(value) && value.length > 0 && value.every((entry) => typeof entry === 'string' && entry.length > 0)) {
    return;
  }
  throw new Error(`Configuration value 'sample.${key}' has an invalid value.`);
}

export { TICTACTOE_SAMPLE_CONFIG, createTicTacToeConfigurationModule };
export type { TicTacToeSampleConfig };

// Hand-written declarations for loomRuntime.js (kept as plain JS -- it's a
// framework-agnostic runtime service, not TS-specific). See loomRuntime.js
// for behavior/comments; this file only describes the public shape so
// consumers of the published package get real types instead of `any`.

export interface LoomRuntime {
  Module: unknown;
  request(method: string, path: string, body?: string): { status: number; body: unknown };
  loadModule(name: string, bytes: Uint8Array | ArrayBuffer): string;
  moduleIds(): string[];
  readNode(nodeId: string): string;
  writeNode(nodeId: string, value: unknown): boolean;
  makeFetch(passthrough?: typeof fetch): typeof fetch;
  installFetch(): () => void;
  stop(): void;
}

export interface CreateLoomRuntimeOptions {
  createModule: () => Promise<unknown>;
  tickMs?: number;
  beforeInit?: (module: unknown) => Promise<void>;
}

export function createLoomRuntime(opts: CreateLoomRuntimeOptions): Promise<LoomRuntime>;

export interface BootFromDataDirOptions {
  createModule: () => Promise<unknown>;
  dataUrl: string;
  moduleUrl: string;
  tickMs?: number;
}

export function bootFromDataDir(
  opts: BootFromDataDirOptions
): Promise<LoomRuntime & { skipped: string[] }>;

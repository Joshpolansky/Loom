// React wrapper around the framework-agnostic Loom-in-wasm runtime service.
//
// Wrap (a subtree of) the app in <LoomRuntimeProvider> and, while it's mounted,
// fetch('/api/*') is served by the in-browser wasm runtime instead of a real
// HTTP server — so the existing REST-driven UI (rest.ts, DataService) works with
// no changes. Live OPC-UA values are a separate transport (not yet faked).
import React, { createContext, useContext, useEffect, useState } from 'react';
// @ts-expect-error — plain-JS service module (see loomRuntime.js)
import { createLoomRuntime } from './loomRuntime.js';

export type LoomRuntime = Awaited<ReturnType<typeof createLoomRuntime>>;

const Ctx = createContext<LoomRuntime | null>(null);
export const useLoomRuntime = (): LoomRuntime | null => useContext(Ctx);

export interface LoomRuntimeProviderProps {
  /** Emscripten factory, e.g. () => (window as any).createLoomModule() or an import. */
  createModule: () => Promise<unknown>;
  /** Optional modules to load at boot. */
  modules?: { name: string; bytes: Uint8Array }[];
  /** Rendered while the runtime is booting. */
  fallback?: React.ReactNode;
  children: React.ReactNode;
}

export function LoomRuntimeProvider({
  createModule,
  modules = [],
  fallback = <div className="loom-booting">Booting Loom runtime…</div>,
  children,
}: LoomRuntimeProviderProps) {
  const [rt, setRt] = useState<LoomRuntime | null>(null);

  useEffect(() => {
    let uninstall = () => {};
    let runtime: LoomRuntime | null = null;
    let cancelled = false;
    (async () => {
      const r = await createLoomRuntime({ createModule });
      if (cancelled) { r.stop(); return; }
      for (const m of modules) r.loadModule(m.name, m.bytes);
      uninstall = r.installFetch(); // route /api/* to the wasm runtime
      runtime = r;
      setRt(r);
    })();
    return () => { cancelled = true; uninstall(); runtime?.stop(); };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  return <Ctx.Provider value={rt}>{rt ? children : fallback}</Ctx.Provider>;
}

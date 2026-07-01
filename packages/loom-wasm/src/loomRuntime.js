// Framework-agnostic "Loom runtime in the browser" service.
//
// Boots the wasm runtime, drives the cooperative tick loop, loads user modules,
// and serves /api/* through the SAME dispatch the native HTTP server uses — so a
// UI built for the native runtime works against the in-browser runtime with no
// changes. The React wrapper is LoomRuntimeProvider.tsx.
//
// Live OPC-UA values (useVariable) are a separate transport, not handled here yet.

/**
 * @param {object} opts
 * @param {() => Promise<any>} opts.createModule  Emscripten factory (createLoomModule).
 * @param {number} [opts.tickMs=25]               Cooperative tick cadence.
 * @param {(M: any) => Promise<void>} [opts.beforeInit]  Called after the wasm
 *   Module is ready and /modules, /data, /uploads exist in MEMFS, but BEFORE
 *   loom_init() runs -- the hook point for seeding instances.json/scheduler.json/
 *   module .so files/config.json into MEMFS ahead of boot (see bootFromDataDir).
 */
export async function createLoomRuntime({ createModule, tickMs = 25, beforeInit } = {}) {
  if (!createModule) throw new Error('createLoomRuntime: createModule (the Emscripten factory) is required');
  const M = await createModule();
  for (const d of ['/modules', '/data', '/uploads']) { try { M.FS.mkdir(d); } catch (e) {} }

  const c = {
    init:       M.cwrap('loom_init', 'number', ['string', 'string']),
    tick:       M.cwrap('loom_tick', null, []),
    request:    M.cwrap('loom_request', 'string', ['string', 'string', 'string']),
    loadModule: M.cwrap('loom_load_module', 'string', ['string']),
    moduleIds:  M.cwrap('loom_module_ids', 'string', []),
    readNode:   M.cwrap('loom_read_node', 'string', ['string']),
    writeNode:  M.cwrap('loom_write_node', 'number', ['string', 'string']),
  };

  if (beforeInit) await beforeInit(M);
  c.init('/modules', '/data');
  let timer = setInterval(() => c.tick(), tickMs);

  /** Route a request through the wasm runtime. @returns {{status:number, body:any}} */
  function request(method, path, body = '') {
    const raw = c.request(String(method).toUpperCase(), path, body || '');
    try { return JSON.parse(raw); } catch (e) { return { status: 502, body: null }; }
  }

  /** Load a user module from raw bytes. @returns {string} module id ('' on failure). */
  function loadModule(name, bytes) {
    const p = '/uploads/' + name;
    M.FS.writeFile(p, bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes));
    return c.loadModule(p);
  }

  /** @returns {string[]} ids of currently loaded modules. */
  function moduleIds() {
    const s = c.moduleIds();
    return s ? s.split(',').filter(Boolean) : [];
  }

  /** A fetch() that serves /api/* from the wasm runtime and delegates the rest. */
  function makeFetch(passthrough = (typeof fetch !== 'undefined' ? fetch.bind(globalThis) : null)) {
    return async (input, init) => {
      const url = typeof input === 'string' ? input : input.url;
      const u = new URL(url, 'http://loom.wasm');
      if (u.pathname.startsWith('/api/')) {
        const method = init && init.method ? init.method : 'GET';
        const body = init && init.body != null ? String(init.body) : '';
        const env = request(method, u.pathname + u.search, body);
        return new Response(env.body == null ? '' : JSON.stringify(env.body), {
          status: env.status,
          headers: { 'Content-Type': 'application/json' },
        });
      }
      if (!passthrough) throw new Error('LoomRuntime: no passthrough fetch for ' + url);
      return passthrough(input, init);
    };
  }

  /** Replace globalThis.fetch so existing fetch('/api/*') hits wasm. @returns uninstaller */
  function installFetch() {
    const orig = globalThis.fetch ? globalThis.fetch.bind(globalThis) : null;
    globalThis.fetch = makeFetch(orig);
    return () => { if (orig) globalThis.fetch = orig; };
  }

  /** Raw reflected-value JSON for an OPC-UA-style nodeId ('null' if unknown). */
  function readNode(nodeId) { return c.readNode(nodeId); }

  /** Write a reflected node value (any JSON-serializable). @returns {boolean} */
  function writeNode(nodeId, value) { return c.writeNode(nodeId, JSON.stringify(value)) === 1; }

  function stop() { if (timer) { clearInterval(timer); timer = null; } }

  return { Module: M, request, loadModule, moduleIds, readNode, writeNode, makeFetch, installFetch, stop };
}

/**
 * Boot the runtime the way native `loom --module-dir <dir> --data-dir <dir>`
 * does: read <dataUrl>/instances.json (the SAME manifest format native reads
 * from disk -- [{id, so, className}, ...]) to learn which modules to load,
 * fetch each one's .so from <moduleUrl>/, and fetch each instance's persisted
 * config.json + the shared scheduler.json from <dataUrl>/, seeding all of it
 * into MEMFS before loom_init() runs. RuntimeCore::loadModules()'s existing
 * native "boot from instances.json" code path does everything else -- this
 * function's only job is turning "files on a real dataDir/moduleDir" into
 * "the same files fetched over HTTP and written into MEMFS first".
 *
 * A module named in instances.json but not found at <moduleUrl>/<so>.so
 * (e.g. it was never built for wasm) is skipped with a warning, matching
 * native's resolveModulePath() behavior -- the rest of the boot proceeds.
 *
 * @param {object} opts
 * @param {() => Promise<any>} opts.createModule  Emscripten factory (createLoomModule).
 * @param {string} opts.dataUrl    Base URL serving the real dataDir's contents
 *   (instances.json, scheduler.json, <id>/config.json, ...) read-only.
 * @param {string} opts.moduleUrl  Base URL serving the real moduleDir's .so files.
 * @param {number} [opts.tickMs=25]
 * @returns {Promise<ReturnType<typeof createLoomRuntime> & {skipped: string[]}>}
 */
export async function bootFromDataDir({ createModule, dataUrl, moduleUrl, tickMs = 25 } = {}) {
  if (!dataUrl || !moduleUrl) throw new Error('bootFromDataDir: dataUrl and moduleUrl are required');
  const skipped = [];
  let manifestIds = [];

  async function fetchOk(url) {
    try {
      // no-store: native boot reads whatever's on disk right now, with no
      // caching layer -- a browser HTTP cache serving a stale build after a
      // module gets rebuilt (or removed) would silently defeat that.
      const r = await fetch(url, { cache: 'no-store' });
      return r.ok ? r : null;
    } catch (e) {
      return null;
    }
  }

  const beforeInit = async (M) => {
    const manifestResp = await fetchOk(`${dataUrl}/instances.json`);
    if (!manifestResp) {
      console.warn(`bootFromDataDir: no instances.json at ${dataUrl} -- booting with zero modules`);
      return;
    }
    const manifestText = await manifestResp.text();
    M.FS.writeFile('/data/instances.json', manifestText);

    let entries = [];
    try { entries = JSON.parse(manifestText); } catch (e) {
      console.warn('bootFromDataDir: instances.json did not parse as JSON', e);
    }
    manifestIds = entries.map((e) => e && e.id).filter(Boolean);

    const schedResp = await fetchOk(`${dataUrl}/scheduler.json`);
    if (schedResp) M.FS.writeFile('/data/scheduler.json', await schedResp.text());

    for (const entry of entries) {
      const { id, so } = entry;
      if (!id || !so) continue;

      const soResp = await fetchOk(`${moduleUrl}/${so}.so`);
      if (!soResp) {
        console.warn(`bootFromDataDir: '${id}' (${so}.so) not found at ${moduleUrl} -- skipping ` +
                     `(likely not built for wasm; native-only modules are expected to be absent here)`);
        skipped.push(id);
        continue;
      }
      const bytes = new Uint8Array(await soResp.arrayBuffer());
      M.FS.writeFile(`/modules/${so}.so`, bytes);

      const cfgResp = await fetchOk(`${dataUrl}/${id}/config.json`);
      if (cfgResp) {
        try { M.FS.mkdir(`/data/${id}`); } catch (e) {}
        M.FS.writeFile(`/data/${id}/config.json`, await cfgResp.text());
      }
    }
  };

  const rt = await createLoomRuntime({ createModule, tickMs, beforeInit });

  // A manifest id whose .so fetched fine (so it's not already in `skipped`)
  // but never ended up registered was rejected inside loom_init() itself --
  // most likely an SDK/ABI version mismatch (see runtime/src/module_loader.cpp,
  // which refuses to load a module built against a different SDK version and
  // logs a clear error, but has no per-module JS-visible return value at this
  // boot stage). Fold it into `skipped` too, distinct from a 404, so a
  // consumer doesn't silently end up with fewer live modules than
  // instances.json listed with no visible signal.
  const loadedIds = new Set(rt.moduleIds());
  for (const id of manifestIds) {
    if (!skipped.includes(id) && !loadedIds.has(id)) {
      console.warn(`bootFromDataDir: '${id}' fetched but did not load (likely SDK/ABI version ` +
                   `mismatch -- check the console above for a "SDK version mismatch" error)`);
      skipped.push(id);
    }
  }

  return { ...rt, skipped };
}

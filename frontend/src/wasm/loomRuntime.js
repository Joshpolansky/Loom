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
 */
export async function createLoomRuntime({ createModule, tickMs = 25 } = {}) {
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

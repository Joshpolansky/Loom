import { OpcuaMachine } from '@loupeteam/lux-connect';

// One OpcuaMachine for the whole app, pointed at the same origin that served the
// UI (the Loom runtime's mapp Connect-compatible facade). Same origin means no
// CORS and cookies are handled by the browser automatically.
const loc = window.location;
const isHttps = loc.protocol === 'https:';
const port = loc.port ? Number(loc.port) : isHttps ? 443 : 80;

export const machine = new OpcuaMachine({
  host: loc.hostname,
  port,
  protocol: isHttps ? 'https' : 'http',
  wsProtocol: isHttps ? 'wss' : 'ws',
  enableWebSocket: true,
});

export function node(moduleId: string, section: string, path?: string): string {
  const base = `ns=1;s=/module/${moduleId}/${section}`;
  if (!path) return base;
  const clean = path.replace(/^\/+/,'').replace(/\/+/g, '/');
  return clean ? `${base}/${clean}` : base;
}

export function classNode(name: string): string {
  return `ns=1;s=/scheduler/classes/${name}`;
}

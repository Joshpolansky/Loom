// WasmMachine — a drop-in for @loupeteam/lux-connect's OpcuaMachine that serves
// useVariable()/writeVariable() from the in-browser Loom runtime instead of an
// OPC-UA server.
//
// lux-react only ever calls the public machine surface (connect, connectionState,
// onConnectionStateChanged, subscribe, unsubscribe, readVariable, writeVariable),
// so this duck-types it. A variable name is an OPC-UA NodeId, e.g.
// "ns=1;s=/module/<id>/runtime/<path>" (see api/machine.ts node()), which the
// host maps to the module's reflected readField/writeField. Subscriptions are
// satisfied by polling the reflected value each tick — no push channel needed.
//
// No dependency on @loupeteam/lux-connect: lux-react's ICommLayer contract
// types connectionState as `ConnectionState | string`, so the plain string
// 'connected' (lux-connect's own ConnectionState.CONNECTED enum member, at
// runtime, is exactly this string) satisfies it without pulling in the
// package just for one enum value.
const CONNECTED = 'connected';

/** The runtime service this machine reads/writes through (see loomRuntime.js). */
export interface NodeAccess {
  readNode(nodeId: string): string;            // raw reflected JSON ('null' if unknown)
  writeNode(nodeId: string, value: unknown): boolean;
}

type StateHandler = (state: string) => void;
interface Sub { nodeId: string; cb: (v: unknown) => void; lastRaw: string; }

export class WasmMachine {
  private readonly rt: NodeAccess;
  private readonly handlers = new Set<StateHandler>();
  private readonly subs = new Map<string, Sub>();
  private nextHandle = 1;
  private readonly poll: ReturnType<typeof setInterval>;

  constructor(rt: NodeAccess, pollMs = 100) {
    this.rt = rt;
    this.poll = setInterval(() => this.tick(), pollMs);
  }

  // --- connection (always "connected": the runtime is in-process) ---
  get connectionState(): string { return CONNECTED; }
  isConnected(): boolean { return true; }
  async connect(): Promise<void> { this.handlers.forEach((h) => h(CONNECTED)); }
  async disconnect(): Promise<void> {}
  async reconnect(): Promise<void> {}
  async testConnection(): Promise<void> {}
  onConnectionStateChanged(handler: StateHandler): void {
    this.handlers.add(handler);
    handler(CONNECTED);
  }

  // --- reads / writes ---
  async readVariable(nodeId: string): Promise<unknown> { return this.parse(this.rt.readNode(nodeId)); }
  async writeVariable(nodeId: string, value: unknown): Promise<void> { this.rt.writeNode(nodeId, value); }

  // --- subscriptions (poll-based; lux-react calls subscribe()/unsubscribe()) ---
  async subscribe(nodeId: string, cb: (v: unknown) => void, _opts?: unknown): Promise<string> {
    const handle = String(this.nextHandle++);
    const lastRaw = this.rt.readNode(nodeId);
    this.subs.set(handle, { nodeId, cb, lastRaw });
    cb(this.parse(lastRaw)); // deliver the initial value immediately
    return handle;
  }
  async unsubscribe(handle: string): Promise<void> { this.subs.delete(handle); }

  dispose(): void { clearInterval(this.poll); this.subs.clear(); this.handlers.clear(); }

  private parse(raw: string): unknown { try { return JSON.parse(raw); } catch { return raw; } }

  private tick(): void {
    for (const s of this.subs.values()) {
      const raw = this.rt.readNode(s.nodeId);
      if (raw !== s.lastRaw) { s.lastRaw = raw; s.cb(this.parse(raw)); }
    }
  }
}

import { useEffect, useState } from 'react';
import { getBusTopics, getBusServices, callBusService } from '../api/rest';
import type { ServiceInfo } from '../types';

// ---------------------------------------------------------------------------
// Tree node type for topic/service trees
// ---------------------------------------------------------------------------
interface BusTreeNode {
  __leaf?: boolean;
  __full?: string;
  __svc?: ServiceInfo;
  [key: string]: BusTreeNode | boolean | string | ServiceInfo | undefined;
}

// ---------------------------------------------------------------------------
// SchemaForm — auto-generates inputs from a JSON Schema properties object
// ---------------------------------------------------------------------------
interface SchemaProps {
  schema: Record<string, unknown>;
  values: Record<string, string>;
  onChange: (key: string, val: string) => void;
}

// Resolve a JSON Schema property def, following $ref into $defs if needed.
function resolveType(def: Record<string, unknown>, defs: Record<string, { type?: unknown }>): string {
  if (def.$ref && typeof def.$ref === 'string') {
    // $ref looks like "#/$defs/double" — extract the name after the last /
    const refName = def.$ref.split('/').pop() ?? '';
    const resolved = defs[refName];
    if (resolved) {
      const t = resolved.type;
      return Array.isArray(t) ? String(t[0]) : String(t ?? 'string');
    }
  }
  const t = def.type;
  return Array.isArray(t) ? String(t[0]) : String(t ?? 'string');
}

function SchemaForm({ schema, values, onChange }: SchemaProps) {
  const defs = (schema.$defs ?? {}) as Record<string, { type?: unknown }>;

  function renderFields(node: Record<string, unknown>, prefix = '') {
    const props = (node.properties ?? {}) as Record<string, Record<string, unknown>>;
    const keys = Object.keys(props || {}).sort((a, b) => a.localeCompare(b));
    return keys.map((key) => {
      let def = props[key];
      // resolve $ref into defs if present
      if (def && def.$ref && typeof def.$ref === 'string') {
        const refName = def.$ref.split('/').pop() ?? '';
        const resolved = defs[refName];
        if (resolved) def = { ...resolved } as Record<string, unknown>;
      }
      const fullKey = prefix ? `${prefix}.${key}` : key;
      const type = resolveType(def, defs);
      if (type === 'object' && def.properties) {
        return (
          <div key={fullKey} className="schema-group">
            <div className="schema-group-title">{key}</div>
            <div className="schema-group-body">{renderFields(def, fullKey)}</div>
          </div>
        );
      }
      if (type === 'array') {
        return (
          <div key={fullKey} className="schema-field">
            <label className="schema-label">{key} (array JSON)</label>
            <textarea className="schema-input" value={values[fullKey] ?? ''} placeholder="[1,2,3]" onChange={(e) => onChange(fullKey, e.target.value)} />
          </div>
        );
      }
      const isBool = type === 'boolean';
      return (
        <div key={fullKey} className="schema-field">
          <label className="schema-label">{key}</label>
          {isBool ? (
            <select className="schema-input" value={values[fullKey] ?? 'false'} onChange={(e) => onChange(fullKey, e.target.value)}>
              <option value="true">true</option>
              <option value="false">false</option>
            </select>
          ) : (
            <input
              className="schema-input"
              type={type === 'number' || type === 'integer' ? 'number' : 'text'}
              value={values[fullKey] ?? ''}
              placeholder={typeof def.description === 'string' ? def.description : type}
              onChange={(e) => onChange(fullKey, e.target.value)}
            />
          )}
        </div>
      );
    });
  }

  // top-level
  const topProps = (schema.properties ?? {}) as Record<string, Record<string, unknown>>;
  if (Object.keys(topProps).length === 0) return null;

  return <div className="schema-form">{renderFields(schema)}</div>;
}

// ---------------------------------------------------------------------------
// BusView
// ---------------------------------------------------------------------------
export default function BusView() {
  const [topics, setTopics] = useState<string[]>([]);
  const [services, setServices] = useState<ServiceInfo[]>([]);
  const [expandedTopics, setExpandedTopics] = useState<Set<string>>(() => new Set());
  const [expandedServices, setExpandedServices] = useState<Set<string>>(() => new Set());
  const [error, setError] = useState('');
  const [selected, setSelected] = useState<ServiceInfo | null>(null);
  const [fieldValues, setFieldValues] = useState<Record<string, string>>({});
  const [rawBody, setRawBody] = useState('{}');
  const [useSchemaForm, setUseSchemaForm] = useState(true);
  const [callResult, setCallResult] = useState('');
  const [calling, setCalling] = useState(false);

  useEffect(() => {
    load();
  }, []);

  function load() {
    Promise.all([getBusTopics(), getBusServices()])
      .then(([t, s]) => { setTopics(t); setServices(s); })
      .catch((e: unknown) => setError(e instanceof Error ? e.message : String(e)));
  }

  // Build a nested tree from a list of slash-delimited paths.
  function buildTree(paths: string[]) {
    const root: BusTreeNode = {};
    for (const p of paths) {
      const parts = p.split('/').filter(Boolean);
      let node = root;
      for (const part of parts) {
        if (!node[part]) node[part] = {};
        node = node[part] as BusTreeNode;
      }
      // mark leaf
      node.__leaf = true;
      node.__full = p;
    }
    return root;
  }

  function buildServiceTree(svcs: ServiceInfo[]) {
    const root: BusTreeNode = {};
    for (const s of svcs) {
      const parts = s.name.split('/').filter(Boolean);
      let node = root;
      for (const part of parts) {
        if (!node[part]) node[part] = {};
        node = node[part] as BusTreeNode;
      }
      node.__leaf = true;
      node.__full = s.name;
      node.__svc = s;
    }
    return root;
  }

  function toggleNode(path: string, isService = false) {
    if (isService) {
      setExpandedServices(prev => { const s = new Set(prev); if (s.has(path)) s.delete(path); else s.add(path); return s; });
    } else {
      setExpandedTopics(prev => { const s = new Set(prev); if (s.has(path)) s.delete(path); else s.add(path); return s; });
    }
  }

  function renderTree(node: BusTreeNode, prefix = '', isService = false) {
    const keys = Object.keys(node).filter(k => !k.startsWith('__')).sort((a, b) => a.localeCompare(b));
    return (
      <ul className="bus-list">
        {keys.map(k => {
          const child = node[k] as BusTreeNode;
          const path = prefix ? `${prefix}/${k}` : k;
          const hasChildren = Object.keys(child).some(x => !x.startsWith('__'));
          const isLeaf = !!child.__leaf && !hasChildren;
          return (
            <li key={path} className={isService && child.__svc ? `bus-service-item${selected?.name === child.__full ? ' selected' : ''}` : undefined}>
              <div style={{ display: 'flex', alignItems: 'center', gap: '0.5rem' }}>
                {hasChildren ? (
                  <button className="bus-toggle" onClick={() => toggleNode(path, isService)} style={{ background: 'none', border: 'none', color: 'inherit' }}>
                    {(isService ? expandedServices : expandedTopics).has(path) ? '▾' : '▸'}
                  </button>
                ) : <span className="bus-toggle"> </span>}
                {isLeaf ? (
                  isService ? (
                    <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', width: '100%' }} onClick={() => { if (child.__svc) selectService(child.__svc as ServiceInfo); }}>
                      <span style={{ fontFamily: 'var(--mono)' }}>{child.__full}</span>
                      {child.__svc && child.__svc.schema && <span className="schema-badge">schema</span>}
                    </div>
                  ) : (
                    <span style={{ fontFamily: 'var(--mono)' }}>{child.__full}</span>
                  )
                ) : (
                  <div style={{ display: 'flex', alignItems: 'center', gap: '0.5rem' }} onClick={() => toggleNode(path)}>
                    <span style={{ fontFamily: 'var(--mono)' }}>{k}</span>
                  </div>
                )}
              </div>
              {hasChildren && (isService ? expandedServices : expandedTopics).has(path) && renderTree(child, path, isService)}
            </li>
          );
        })}
      </ul>
    );
  }

  function selectService(svc: ServiceInfo) {
    setSelected(svc);
    setFieldValues({});
    setRawBody('{}');
    setCallResult('');
    // If schema has properties, default to schema form
    const hasProps = svc.schema && Object.keys((svc.schema.properties ?? {}) as object).length > 0;
    setUseSchemaForm(!!hasProps);
  }

  function buildRequestBody(): string {
    if (!useSchemaForm || !selected?.schema) return rawBody;
    const schema = selected.schema;
    const defs = (schema.$defs ?? {}) as Record<string, { type?: unknown }>;
    const out: Record<string, unknown> = {};

    function setNested(obj: Record<string, unknown>, parts: string[], val: unknown) {
      const [head, ...rest] = parts;
      if (rest.length === 0) {
        obj[head] = val;
        return;
      }
      if (!obj[head] || typeof obj[head] !== 'object') obj[head] = {};
      setNested(obj[head] as Record<string, unknown>, rest, val);
    }

    function findDef(node: Record<string, unknown>, parts: string[]): Record<string, unknown> | null {
      if (parts.length === 0) return node;
      const [head, ...rest] = parts;
      const props = (node.properties ?? {}) as Record<string, Record<string, unknown>>;
      const def = props?.[head];
      if (!def) return null;
      // resolve $ref if necessary
      let resolvedDef = def;
      if (def.$ref && typeof def.$ref === 'string') {
        const refName = def.$ref.split('/').pop() ?? '';
        const r = defs[refName];
        if (r) resolvedDef = r as Record<string, unknown>;
      }
      if (rest.length === 0) return resolvedDef;
      return findDef(resolvedDef, rest);
    }

    for (const [dotKey, raw] of Object.entries(fieldValues)) {
      const parts = dotKey.split('.');
      const def = findDef(schema, parts);
      const type = def ? resolveType(def, defs) : 'string';
      let val: unknown = raw;
      if (type === 'number' || type === 'integer') val = raw === '' ? 0 : Number(String(raw));
      else if (type === 'boolean') val = String(raw) === 'true';
      else if (type === 'array') {
        try { val = JSON.parse(String(raw)); } catch { val = [] as unknown; }
      } else if (type === 'object') {
        try { val = JSON.parse(String(raw)); } catch { val = {}; }
      }
      setNested(out, parts, val);
    }

    return JSON.stringify(out);
  }

  async function handleCall() {
    if (!selected) return;
    setCalling(true);
    setCallResult('');
    try {
      const result = await callBusService(selected.name, buildRequestBody());
      setCallResult(JSON.stringify(result, null, 2));
    } catch (e: unknown) {
      setCallResult(e instanceof Error ? e.message : String(e));
    } finally {
      setCalling(false);
    }
  }

  const hasSchema = selected?.schema &&
    Object.keys((selected.schema.properties ?? {}) as object).length > 0;

  return (
    <div className="detail-layout">
      <div style={{ display: 'flex', alignItems: 'center', gap: '1rem', marginBottom: '1rem' }}>
        <h2 style={{ margin: 0, color: '#fff' }}>Bus</h2>
        <button className="btn" onClick={load}>Refresh</button>
      </div>

      {error && <div className="error-msg">{error}</div>}

      <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '1rem', alignItems: 'start' }}>
        {/* Topics */}
        <div className="bus-section">
          <h3>Topics ({topics.length})</h3>
          {topics.length === 0 ? (
            <p style={{ color: '#7a7a8e' }}>No active topics</p>
          ) : (
            renderTree(buildTree(topics), 'topic', false)
          )}
        </div>

        {/* Services */}
        <div className="bus-section">
          <h3>Services ({services.length})</h3>
          {services.length === 0 ? (
            <p style={{ color: '#7a7a8e' }}>No registered services</p>
          ) : (
            renderTree(buildServiceTree(services), 'service', true)
          )}
        </div>
      </div>

      {/* Call panel */}
      {selected && (
        <div className="bus-section" style={{ marginTop: '1rem' }}>
          <div style={{ display: 'flex', alignItems: 'baseline', gap: '0.75rem', marginBottom: '0.75rem' }}>
            <h3 style={{ margin: 0 }}>Call: <code>{selected.name}</code></h3>
            {hasSchema && (
              <button
                className="btn"
                style={{ fontSize: '0.75rem', padding: '2px 8px' }}
                onClick={() => setUseSchemaForm((v) => !v)}
              >
                {useSchemaForm ? 'Switch to JSON' : 'Switch to Form'}
              </button>
            )}
          </div>

          {useSchemaForm && hasSchema ? (
            <SchemaForm
              schema={selected.schema!}
              values={fieldValues}
              onChange={(k, v) => setFieldValues((prev) => ({ ...prev, [k]: v }))}
            />
          ) : (
            <textarea
              value={rawBody}
              onChange={(e) => setRawBody(e.target.value)}
              rows={3}
              className="bus-textarea"
            />
          )}

          <div style={{ display: 'flex', gap: '0.5rem', marginTop: '0.75rem' }}>
            <button className="btn primary" onClick={handleCall} disabled={calling}>
              {calling ? 'Calling…' : 'Call'}
            </button>
          </div>

          {callResult && (
            <pre className="bus-result">{callResult}</pre>
          )}
        </div>
      )}
    </div>
  );
}

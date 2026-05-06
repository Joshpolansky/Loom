import { useEffect, useState } from 'react';
import { getBusServices, callBusService } from '../api/rest';
import type { ServiceInfo } from '../types';

// ---------------------------------------------------------------------------
// Helpers — mirrors resolveType / SchemaForm from BusView (kept local)
// ---------------------------------------------------------------------------
function resolveType(
  def: Record<string, unknown>,
  defs: Record<string, { type?: unknown }>,
): string {
  if (def.$ref && typeof def.$ref === 'string') {
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

interface SchemaFormProps {
  schema: Record<string, unknown>;
  values: Record<string, string>;
  onChange: (key: string, val: string) => void;
}

function SchemaForm({ schema, values, onChange }: SchemaFormProps) {
  const defs = (schema.$defs ?? {}) as Record<string, { type?: unknown }>;

  function renderFields(node: Record<string, unknown>, prefix = ''): React.ReactNode {
    const props = (node.properties ?? {}) as Record<string, Record<string, unknown>>;
    return Object.keys(props).sort().map((key) => {
      let def = props[key];
      if (def.$ref && typeof def.$ref === 'string') {
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
            <label className="schema-label">{key} (JSON)</label>
            <textarea className="schema-input" value={values[fullKey] ?? ''} placeholder="[]"
              onChange={(e) => onChange(fullKey, e.target.value)} />
          </div>
        );
      }
      const isBool = type === 'boolean';
      return (
        <div key={fullKey} className="schema-field">
          <label className="schema-label">{key}</label>
          {isBool ? (
            <select className="schema-input" value={values[fullKey] ?? 'false'}
              onChange={(e) => onChange(fullKey, e.target.value)}>
              <option value="true">true</option>
              <option value="false">false</option>
            </select>
          ) : (
            <input className="schema-input"
              type={type === 'number' || type === 'integer' ? 'number' : 'text'}
              value={values[fullKey] ?? ''}
              placeholder={typeof def.description === 'string' ? def.description : type}
              onChange={(e) => onChange(fullKey, e.target.value)} />
          )}
        </div>
      );
    });
  }

  const topProps = (schema.properties ?? {}) as Record<string, unknown>;
  if (Object.keys(topProps).length === 0) return <p className="rpc-empty">No parameters</p>;
  return <div className="schema-form">{renderFields(schema)}</div>;
}

// ---------------------------------------------------------------------------
// ModuleRpcPanel
// ---------------------------------------------------------------------------
interface Props {
  moduleId: string;
}

export function ModuleRpcPanel({ moduleId }: Props) {
  const [services, setServices] = useState<ServiceInfo[]>([]);
  const [selected, setSelected] = useState<ServiceInfo | null>(null);
  const [fieldValues, setFieldValues] = useState<Record<string, string>>({});
  const [rawBody, setRawBody] = useState('{}');
  const [useSchema, setUseSchema] = useState(true);
  const [calling, setCalling] = useState(false);
  const [result, setResult] = useState('');
  const [loadError, setLoadError] = useState('');

  useEffect(() => {
    getBusServices()
      .then((all) => setServices(all.filter((s) => s.name.startsWith(`${moduleId}/`))))
      .catch((e: unknown) => setLoadError(e instanceof Error ? e.message : String(e)));
  }, [moduleId]);

  function selectService(svc: ServiceInfo) {
    setSelected(svc);
    setFieldValues({});
    setRawBody('{}');
    setResult('');
    const hasProps = svc.schema && Object.keys((svc.schema.properties ?? {}) as object).length > 0;
    setUseSchema(!!hasProps);
  }

  function buildBody(): string {
    if (!useSchema || !selected?.schema) return rawBody;
    const schema = selected.schema;
    const defs = (schema.$defs ?? {}) as Record<string, { type?: unknown }>;
    const out: Record<string, unknown> = {};

    function setNested(obj: Record<string, unknown>, parts: string[], val: unknown) {
      const [head, ...rest] = parts;
      if (rest.length === 0) { obj[head] = val; return; }
      if (!obj[head] || typeof obj[head] !== 'object') obj[head] = {};
      setNested(obj[head] as Record<string, unknown>, rest, val);
    }

    function findDef(node: Record<string, unknown>, parts: string[]): Record<string, unknown> | null {
      if (parts.length === 0) return node;
      const [head, ...rest] = parts;
      const props = (node.properties ?? {}) as Record<string, Record<string, unknown>>;
      let def = props?.[head];
      if (!def) return null;
      if (def.$ref && typeof def.$ref === 'string') {
        const refName = def.$ref.split('/').pop() ?? '';
        const r = defs[refName];
        if (r) def = { ...r } as Record<string, unknown>;
      }
      if (rest.length === 0) return def;
      return findDef(def, rest);
    }

    for (const [dotKey, raw] of Object.entries(fieldValues)) {
      const parts = dotKey.split('.');
      const def = findDef(schema, parts);
      const type = def ? resolveType(def, defs) : 'string';
      let val: unknown = raw;
      if (type === 'number' || type === 'integer') val = raw === '' ? 0 : Number(raw);
      else if (type === 'boolean') val = raw === 'true';
      else if (type === 'array') { try { val = JSON.parse(raw); } catch { val = []; } }
      else if (type === 'object') { try { val = JSON.parse(raw); } catch { val = {}; } }
      setNested(out, parts, val);
    }
    return JSON.stringify(out);
  }

  async function handleCall() {
    if (!selected) return;
    setCalling(true);
    setResult('');
    try {
      const r = await callBusService(selected.name, buildBody());
      setResult(JSON.stringify(r, null, 2));
    } catch (e: unknown) {
      setResult(e instanceof Error ? e.message : String(e));
    } finally {
      setCalling(false);
    }
  }

  if (loadError) return <div className="error-msg">{loadError}</div>;

  if (services.length === 0) {
    return <p className="rpc-empty">No services registered for this module.</p>;
  }

  const shortName = (svc: ServiceInfo) => svc.name.slice(moduleId.length + 1);
  const hasSchemaProps = selected?.schema &&
    Object.keys((selected.schema.properties ?? {}) as object).length > 0;

  return (
    <div className="rpc-panel">
      {/* Service list */}
      <div className="rpc-list">
        {services.map((svc) => (
          <div
            key={svc.name}
            className={`rpc-item${selected?.name === svc.name ? ' selected' : ''}`}
            onClick={() => selectService(svc)}
          >
            <span className="rpc-name">{shortName(svc)}</span>
            {svc.schema && <span className="schema-badge">schema</span>}
          </div>
        ))}
      </div>

      {/* Call panel */}
      {selected && (
        <div className="rpc-call">
          <div className="rpc-call-header">
            <span className="rpc-call-title">{selected.name}</span>
            {hasSchemaProps && (
              <button className="btn" style={{ fontSize: '0.75rem', padding: '0.2rem 0.5rem' }}
                onClick={() => setUseSchema((v) => !v)}>
                {useSchema ? 'Raw JSON' : 'Form'}
              </button>
            )}
          </div>

          {useSchema && selected.schema ? (
            <SchemaForm
              schema={selected.schema}
              values={fieldValues}
              onChange={(k, v) => setFieldValues((prev) => ({ ...prev, [k]: v }))}
            />
          ) : (
            <textarea
              className="rpc-raw-input"
              value={rawBody}
              onChange={(e) => setRawBody(e.target.value)}
              rows={4}
              spellCheck={false}
            />
          )}

          <button className="btn primary" onClick={handleCall} disabled={calling}>
            {calling ? 'Calling…' : 'Call'}
          </button>

          {result && (
            <pre className="rpc-result">{result}</pre>
          )}
        </div>
      )}
    </div>
  );
}

import type {
  ModuleInfo,
  ModuleDetail,
  ServiceCallResult,
  ServiceInfo,
  DataSection,
  ClassInfo,
  ProbeInfo,
  MetricSample,
} from '../types';

export interface HistoryResponse {
  samples: MetricSample[];
  latest: number;
}

const BASE = '/api';

async function fetchJson<T>(url: string, init?: RequestInit): Promise<T> {
  const res = await fetch(url, init);
  if (!res.ok) throw new Error(`${res.status} ${res.statusText}`);
  return res.json();
}

export async function getModules(): Promise<ModuleInfo[]> {
  return fetchJson(`${BASE}/modules`);
}

export async function getModule(id: string): Promise<ModuleDetail> {
  return fetchJson(`${BASE}/modules/${encodeURIComponent(id)}`);
}

export async function getModuleData(
  id: string,
  section: DataSection,
): Promise<Record<string, unknown>> {
  return fetchJson(`${BASE}/modules/${encodeURIComponent(id)}/data/${section}`);
}

export async function setModuleData(
  id: string,
  section: DataSection,
  data: Record<string, unknown>,
): Promise<{ ok: boolean }> {
  return fetchJson(
    `${BASE}/modules/${encodeURIComponent(id)}/data/${section}`,
    {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(data),
    },
  );
}

export async function patchModuleData(
  id: string,
  section: DataSection,
  ptr: string,
  value: unknown,
): Promise<{ ok: boolean }> {
  return fetchJson(
    `${BASE}/modules/${encodeURIComponent(id)}/data/${section}`,
    {
      method: 'PATCH',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ ptr, value }),
    },
  );
}

export async function reloadModule(
  id: string,
): Promise<{ ok: boolean; message?: string }> {
  return fetchJson(
    `${BASE}/modules/${encodeURIComponent(id)}/reload`,
    { method: 'POST' },
  );
}

export async function saveModuleConfig(id: string): Promise<{ ok: boolean }> {
  return fetchJson(`${BASE}/modules/${encodeURIComponent(id)}/config/save`, { method: 'POST' });
}

export async function loadModuleConfig(id: string): Promise<Record<string, unknown>> {
  return fetchJson(`${BASE}/modules/${encodeURIComponent(id)}/config/load`, { method: 'POST' });
}

export async function saveModuleRecipe(id: string, name: string): Promise<{ ok: boolean }> {
  return fetchJson(`${BASE}/modules/${encodeURIComponent(id)}/recipe/save/${encodeURIComponent(name)}`, { method: 'POST' });
}

export async function loadModuleRecipe(id: string, name: string): Promise<Record<string, unknown>> {
  return fetchJson(`${BASE}/modules/${encodeURIComponent(id)}/recipe/load/${encodeURIComponent(name)}`, { method: 'POST' });
}

export async function instantiateModule(so: string, id: string): Promise<{ ok: boolean; id: string }> {
  return fetchJson(`${BASE}/modules/instantiate`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ so, id }),
  });
}

export async function removeModuleInstance(id: string): Promise<{ ok: boolean }> {
  return fetchJson(`${BASE}/modules/${encodeURIComponent(id)}`, { method: 'DELETE' });
}

export interface AvailableModule {
  filename: string;
  className: string;
  version: string;
}

export async function getAvailableModules(): Promise<AvailableModule[]> {
  return fetchJson(`${BASE}/modules/available`);
}

export async function getBusTopics(): Promise<string[]> {
  return fetchJson(`${BASE}/bus/topics`);
}

export async function getSchedulerClasses(): Promise<ClassInfo[]> {
  return fetchJson(`${BASE}/scheduler/classes`);
}

export async function getModuleHistory(id: string, since = 0, binMs = 0): Promise<HistoryResponse> {
  const bin = binMs > 0 ? `&bin=${binMs}` : '';
  return fetchJson(`${BASE}/scheduler/modules/${encodeURIComponent(id)}/history?since=${since}${bin}`);
}

export async function getClassHistory(name: string, since = 0, binMs = 0): Promise<HistoryResponse> {
  const bin = binMs > 0 ? `&bin=${binMs}` : '';
  return fetchJson(`${BASE}/scheduler/classes/${encodeURIComponent(name)}/history?since=${since}${bin}`);
}

export async function updateClassDef(
  name: string,
  patch: Partial<Pick<ClassInfo, 'period_us' | 'priority' | 'cpu_affinity' | 'spin_us'>>,
): Promise<{ ok: boolean }> {
  return fetchJson(`${BASE}/scheduler/classes/${encodeURIComponent(name)}`, {
    method: 'PATCH',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(patch),
  });
}

export async function createClass(
  def: Pick<ClassInfo, 'name' | 'period_us' | 'priority' | 'cpu_affinity' | 'spin_us'>,
): Promise<{ ok: boolean }> {
  return fetchJson(`${BASE}/scheduler/classes`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(def),
  });
}

export async function reassignModuleClass(
  moduleId: string,
  className: string,
  order?: number,
): Promise<{ ok: boolean }> {
  return fetchJson(`${BASE}/scheduler/reassign`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ moduleId, class: className, ...(order != null ? { order } : {}) }),
  });
}

export async function getBusServices(): Promise<ServiceInfo[]> {
  return fetchJson(`${BASE}/bus/services`);
}

export async function callBusService(
  name: string,
  request: string = '{}',
): Promise<ServiceCallResult> {
  // Preserve '/' separators between module ID and service name; encode only each segment.
  const encodedName = name.split('/').map(encodeURIComponent).join('/');
  return fetchJson(`${BASE}/bus/call/${encodedName}`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: request,
  });
}

// --- Oscilloscope ---

/** Returns `{ moduleId: <runtime JSON object>, ... }`. The frontend walks each
 *  module's tree to present probeable (numeric) fields. */
export async function getScopeSchema(): Promise<Record<string, unknown>> {
  return fetchJson(`${BASE}/scope/schema`);
}

export async function getScopeProbes(): Promise<ProbeInfo[]> {
  return fetchJson(`${BASE}/scope/probes`);
}

export async function addScopeProbe(
  moduleId: string,
  path: string,
): Promise<{ ok: boolean; id: number }> {
  return fetchJson(`${BASE}/scope/probes`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ moduleId, path }),
  });
}

export async function removeScopeProbe(id: number): Promise<{ ok: boolean }> {
  return fetchJson(`${BASE}/scope/probes/${id}`, { method: 'DELETE' });
}

export async function getScopeData(): Promise<Record<string, [number, number][]>> {
  return fetchJson(`${BASE}/scope/data`);
}

// --- Module upload ---

export async function uploadModule(file: File): Promise<{ ok: boolean; id?: string; error?: string }> {
  const res = await fetch(`${BASE}/modules/upload`, {
    method: 'POST',
    headers: { 'X-Filename': file.name },
    body: file,
  });
  if (!res.ok) throw new Error(`${res.status} ${res.statusText}`);
  return res.json();
}

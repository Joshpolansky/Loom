import { useEffect, useLayoutEffect, useRef, useState } from 'react';
import type { LiveUpdate } from '../types';

type LiveCallback = (data: LiveUpdate) => void;

export function useWebSocket(onMessage: LiveCallback) {
  const wsRef = useRef<WebSocket | null>(null);
  const cbRef = useRef(onMessage);
  const connectRef = useRef<(() => void) | null>(null);
  const [connected, setConnected] = useState(false);

  useLayoutEffect(() => {
    cbRef.current = onMessage;
  });

  useEffect(() => {
    function connect() {
      if (wsRef.current?.readyState === WebSocket.OPEN) return;

      const proto = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
      const url = `${proto}//${window.location.host}/ws`;
      const ws = new WebSocket(url);

      ws.onopen = () => setConnected(true);
      ws.onclose = () => {
        setConnected(false);
        setTimeout(() => connectRef.current?.(), 2000);
      };
      ws.onerror = () => ws.close();
      ws.onmessage = (ev) => {
        try {
          const data = JSON.parse(ev.data) as LiveUpdate;
          cbRef.current(data);
        } catch {
          // ignore malformed messages
        }
      };

      wsRef.current = ws;
    }

    connectRef.current = connect;
    connect();
    return () => {
      wsRef.current?.close();
    };
  }, []);

  return { connected };
}

// SPDX-License-Identifier: GPL-3.0-or-later
// A minimal observable store and a hook that re-renders a component when its selection changes.
import { useEffect, useReducer, useRef } from 'preact/hooks';

export class Store<T extends object> {
  private listeners = new Set<() => void>();
  constructor(public state: T) {}
  get(): T {
    return this.state;
  }
  set(patch: Partial<T> | ((s: T) => Partial<T>)) {
    const p = typeof patch === 'function' ? patch(this.state) : patch;
    this.state = { ...this.state, ...p };
    this.listeners.forEach((l) => l());
  }
  subscribe(l: () => void) {
    this.listeners.add(l);
    return () => this.listeners.delete(l);
  }
}

const same = (a: unknown, b: unknown) => {
  if (Object.is(a, b)) return true;
  if (Array.isArray(a) && Array.isArray(b)) return a.length === b.length && a.every((x, i) => Object.is(x, b[i]));
  return false;
};

/** re-render when select(state) changes (shallow for arrays) */
export function useStore<T extends object, R>(store: Store<T>, select: (s: T) => R): R {
  const [, force] = useReducer((x: number) => x + 1, 0);
  const sel = useRef(select);
  sel.current = select;
  const last = useRef<R>(select(store.state));
  last.current = select(store.state);
  useEffect(() => {
    const check = () => {
      const next = sel.current(store.state);
      if (!same(next, last.current)) {
        last.current = next;
        force(0);
      }
    };
    const unsub = store.subscribe(check);
    // effects run after paint: the store may have changed since this component rendered
    check();
    return unsub;
  }, [store]);
  return last.current;
}

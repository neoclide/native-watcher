export type EventType = 'create' | 'update' | 'delete';
export type EntryKind = 'file' | 'directory';

export interface WatchEvent {
  /** Absolute path of the changed entry. */
  path: string;
  type: EventType;
  /** Entry type, retained for deletion events after the path is gone. */
  kind: EntryKind;
  /**
   * Opaque id present on both the delete and create sides when the backend can
   * correlate an unambiguous rename. Do not parse or persist this value.
   */
  renameId?: string;
}

export interface WatchOptions {
  /** Paths, glob patterns, or flag-free regular expressions to exclude. */
  ignore?: Array<string | RegExp>;
}

export interface Subscription {
  unsubscribe(): Promise<void>;
}

export function subscribe(
  /** Directory to watch recursively. Relative paths use process.cwd(). */
  directory: string,
  callback: (error: Error | null, events: WatchEvent[]) => void,
  options?: WatchOptions,
): Promise<Subscription>;

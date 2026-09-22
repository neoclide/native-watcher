export type EventType = 'create' | 'update' | 'delete';

export interface WatchEvent {
  path: string;
  type: EventType;
  /** Present on both sides when the native backend reliably correlates a rename. */
  renameId?: string;
}

export interface WatchOptions {
  ignore?: Array<string | RegExp>;
}

export interface Subscription {
  unsubscribe(): Promise<void>;
}

export function subscribe(
  directory: string,
  callback: (error: Error | null, events: WatchEvent[]) => void,
  options?: WatchOptions,
): Promise<Subscription>;

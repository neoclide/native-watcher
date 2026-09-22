'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const watcher = require('..');

const TIMEOUT = 5000;

class EventCollector {
  constructor() {
    this.events = [];
    this.batches = [];
    this.error = null;
    this.waiters = new Set();
    this.callback = (error, events) => {
      if (error) this.error = error;
      if (events) {
        this.batches.push(events);
        this.events.push(...events);
      }
      for (const notify of this.waiters) notify();
    };
  }

  mark() {
    return this.events.length;
  }

  batchMark() {
    return this.batches.length;
  }

  async waitFrom(mark, predicate, timeout = TIMEOUT) {
    return new Promise((resolve, reject) => {
      const check = () => {
        if (this.error) {
          cleanup();
          reject(this.error);
          return;
        }

        const events = this.events.slice(mark);
        if (predicate(events)) {
          cleanup();
          resolve(events);
        }
      };
      const timer = setTimeout(() => {
        cleanup();
        reject(
          new Error(
            `timed out waiting for events; received ${JSON.stringify(this.events.slice(mark))}`,
          ),
        );
      }, timeout);
      const cleanup = () => {
        clearTimeout(timer);
        this.waiters.delete(check);
      };

      this.waiters.add(check);
      check();
    });
  }

  waitFor(type, eventPath, mark = this.mark()) {
    return this.waitFrom(mark, (events) =>
      events.some((event) => event.type === type && event.path === eventPath),
    );
  }
}

async function createFixture(t, options, prepare) {
  const tempDirectory = await fs.realpath(os.tmpdir());
  const directory = await fs.mkdtemp(
    path.join(tempDirectory, 'native-watcher-test-'),
  );
  if (prepare) await prepare(directory);

  const collector = new EventCollector();
  const subscription = await watcher.subscribe(
    directory,
    collector.callback,
    options,
  );

  t.after(async () => {
    await subscription.unsubscribe();
    await fs.rm(directory, {recursive: true, force: true});
  });

  return {directory, collector, subscription};
}

function assertEvent(events, type, eventPath) {
  assert.ok(
    events.some((event) => event.type === type && event.path === eventPath),
    `missing ${type} event for ${eventPath}: ${JSON.stringify(events)}`,
  );
}

function assertNoPath(events, ignoredPath) {
  const prefix = `${ignoredPath}${path.sep}`;
  assert.ok(
    events.every(
      (event) => event.path !== ignoredPath && !event.path.startsWith(prefix),
    ),
    `received event below ignored path ${ignoredPath}: ${JSON.stringify(events)}`,
  );
}

module.exports = {
  EventCollector,
  assertEvent,
  assertNoPath,
  createFixture,
};

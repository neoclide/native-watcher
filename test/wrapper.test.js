'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const os = require('node:os');
const test = require('node:test');
const {createWrapper} = require('../wrapper');

test('concurrent unsubscribe calls wait for the same native stop', async () => {
  let finishStop;
  const nativeStop = new Promise((resolve) => { finishStop = resolve; });
  let stopCalls = 0;
  const watcher = createWrapper({
    subscribe: async () => {},
    unsubscribe: () => {
      stopCalls++;
      return nativeStop;
    },
  });
  const subscription = await watcher.subscribe(
    await fs.realpath(os.tmpdir()),
    () => {},
  );

  const first = subscription.unsubscribe();
  const second = subscription.unsubscribe();
  assert.strictEqual(second, first);
  assert.equal(stopCalls, 1);

  let completed = false;
  void second.then(() => { completed = true; });
  await Promise.resolve();
  assert.equal(completed, false);

  finishStop();
  await Promise.all([first, second]);
  assert.equal(completed, true);
});

test('unsubscribe keeps a synchronous native failure as one rejected promise', async () => {
  let stopCalls = 0;
  const watcher = createWrapper({
    subscribe: async () => {},
    unsubscribe: () => {
      stopCalls++;
      throw new Error('stop failed');
    },
  });
  const subscription = await watcher.subscribe(
    await fs.realpath(os.tmpdir()),
    () => {},
  );
  const first = subscription.unsubscribe();
  assert.strictEqual(subscription.unsubscribe(), first);
  await assert.rejects(first, /stop failed/);
  assert.equal(stopCalls, 1);
});

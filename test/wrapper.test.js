'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const {createWrapper} = require('../wrapper');
const binding = require('../build/Release/native_watcher.node');

test('passes raw star patterns to native code and keeps other punctuation literal', async () => {
  let options;
  const watcher = createWrapper({
    subscribe: async (_directory, _callback, value) => { options = value; },
    unsubscribe: async () => {},
  });
  const directory = await fs.realpath(os.tmpdir());
  const subscription = await watcher.subscribe(directory, () => {}, {
    ignore: ['[x]*.log', 'cache'],
  });
  assert.deepEqual(options.ignoreGlobs, ['[x]*.log', 'cache']);
  assert.equal(options.ignorePaths, undefined);
  await subscription.unsubscribe();
});

test('rejects RegExp and non-string ignore entries before native subscription', async () => {
  let calls = 0;
  const watcher = createWrapper({
    subscribe: async () => { calls++; },
    unsubscribe: async () => {},
  });
  const directory = await fs.realpath(os.tmpdir());
  await assert.rejects(
    watcher.subscribe(directory, () => {}, {ignore: [/cache/]}),
    {name: 'TypeError', message: 'Expected ignore patterns to be strings'},
  );
  await assert.rejects(
    watcher.subscribe(directory, () => {}, {ignore: [1]}),
    {name: 'TypeError', message: 'Expected ignore patterns to be strings'},
  );
  await assert.rejects(
    watcher.subscribe(directory, () => {}, {ignore: ['cache\0']}),
    {name: 'TypeError', message: 'ignore patterns cannot contain NUL characters'},
  );
  assert.equal(calls, 0);
});

test('native boundary rejects malformed raw globs before a later subscription', async () => {
  const directory = await fs.mkdtemp(path.join(os.tmpdir(), 'native-watcher-glob-options-'));
  const callback = () => {};
  try {
    assert.throws(
      () => binding.subscribe(directory, callback, {ignoreGlobs: ['ok', 1]}),
      {name: 'TypeError', message: 'Expected ignoreGlobs to be an array of strings'},
    );
    assert.throws(
      () => binding.subscribe(directory, callback, {ignoreGlobs: ['bad\0*']}),
      {name: 'TypeError', message: 'ignoreGlobs cannot contain NUL characters'},
    );
    await binding.subscribe(directory, callback, {ignoreGlobs: ['ignored*']});
    await binding.unsubscribe(directory, callback, {ignoreGlobs: ['ignored*']});
  } finally {
    await fs.rm(directory, {recursive: true, force: true});
  }
});

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

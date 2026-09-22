'use strict';

const assert = require('node:assert/strict');
const fsSync = require('node:fs');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const watcher = require('../..');

async function main() {
  const directory = await fs.mkdtemp(
    path.join(await fs.realpath(os.tmpdir()), 'native-watcher-inotify-start-'),
  );
  const descriptors = [];
  try {
    try {
      while (true) descriptors.push(fsSync.openSync('/dev/null', 'r'));
    } catch (error) {
      assert.equal(error.code, 'EMFILE');
    }

    // Leave exactly enough descriptors for pipe2(). inotify_init1() must then
    // fail with EMFILE after the backend thread consumes both slots.
    fsSync.closeSync(descriptors.pop());
    fsSync.closeSync(descriptors.pop());

    let timerFired = false;
    const timer = new Promise((resolve) => setTimeout(() => {
      timerFired = true;
      resolve();
    }, 0));

    await assert.rejects(
      watcher.subscribe(directory, () => {}, {backend: 'inotify'}),
      /Unable to initialize inotify: Too many open files/,
    );
    await timer;
    assert.equal(timerFired, true);
  } finally {
    for (const descriptor of descriptors) fsSync.closeSync(descriptor);
    await fs.rm(directory, {recursive: true, force: true});
  }
}

main().then(
  () => console.log('inotify startup failure handled'),
  (error) => {
    console.error(error);
    process.exitCode = 1;
  },
);

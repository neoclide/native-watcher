'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const watcher = require('../..');
const {EventCollector} = require('../helpers');

async function main() {
  const mode = process.argv[2];
  assert.ok(mode === 'einval' || mode === 'eio');
  const directory = await fs.mkdtemp(
    path.join(await fs.realpath(os.tmpdir()), 'native-watcher-rm-watch-'),
  );
  await fs.mkdir(path.join(directory, 'child'));

  try {
    if (mode === 'eio') {
      const subscription = await watcher.subscribe(
        directory,
        () => {},
        {backend: 'inotify'},
      );
      await assert.rejects(
        subscription.unsubscribe(),
        /Unable to remove watcher:/,
      );
      return;
    }

    const first = await watcher.subscribe(
      directory,
      () => {},
      {backend: 'inotify'},
    );
    await first.unsubscribe();

    const marker = path.join(directory, 'after-resubscribe');
    const collector = new EventCollector();
    const second = await watcher.subscribe(
      directory,
      collector.callback,
      {backend: 'inotify'},
    );
    try {
      const observed = collector.waitFor('create', marker);
      await fs.writeFile(marker, 'event');
      await observed;
    } finally {
      await second.unsubscribe();
    }
  } finally {
    await fs.rm(directory, {recursive: true, force: true});
  }
}

main().then(
  () => console.log('inotify rm_watch unsubscribe fixture ok'),
  (error) => {
    console.error(error);
    process.exitCode = 1;
  },
);

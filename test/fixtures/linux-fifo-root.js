'use strict';

const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const {execFile} = require('node:child_process');
const {promisify} = require('node:util');
const watcher = require('../..');

const execFileAsync = promisify(execFile);

async function main() {
  const root = await fs.mkdtemp(
    path.join(await fs.realpath(os.tmpdir()), 'native-watcher-fifo-root-'),
  );
  const directory = path.join(root, 'valid');
  const fifo = path.join(root, 'fifo');
  const marker = path.join(directory, 'marker');
  await fs.mkdir(directory);
  await execFileAsync('mkfifo', [fifo]);

  let resolveMarker;
  const markerObserved = new Promise((resolve) => {
    resolveMarker = resolve;
  });
  const subscription = await watcher.subscribe(directory, (error, events) => {
    if (error) throw error;
    if (events.some((event) => event.path === marker)) resolveMarker();
  });

  try {
    await watcher.subscribe(fifo, () => {}).then(
      () => { throw new Error('FIFO subscription unexpectedly succeeded'); },
      (error) => {
        if (!/Not a directory/.test(error.message)) throw error;
      },
    );
    await fs.writeFile(marker, 'event');
    await markerObserved;
  } finally {
    await subscription.unsubscribe();
    await fs.rm(root, {recursive: true, force: true});
  }
}

main().then(
  () => console.log('FIFO root rejected without blocking backend'),
  (error) => {
    console.error(error);
    process.exitCode = 1;
  },
);

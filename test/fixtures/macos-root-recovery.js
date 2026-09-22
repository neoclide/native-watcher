'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const watcher = require('../..');

function waitFor(check, description) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(
      () => reject(new Error(`timed out waiting for ${description}`)),
      4000,
    );
    const poll = () => {
      try {
        if (check()) {
          clearTimeout(timer);
          resolve();
        } else {
          setImmediate(poll);
        }
      } catch (error) {
        clearTimeout(timer);
        reject(error);
      }
    };
    poll();
  });
}

async function main() {
  const mode = process.argv[2];
  const parent = await fs.mkdtemp(
    path.join(await fs.realpath(os.tmpdir()), 'native-watcher-root-recovery-'),
  );
  const root = path.join(parent, 'root');
  const keeperRoot = path.join(parent, 'keeper');
  await fs.mkdir(root);
  if (mode === 'same-options') await fs.mkdir(keeperRoot);

  const deleted = [];
  const first = await watcher.subscribe(root, (error, events) => {
    if (error) throw error;
    deleted.push(...(events || []));
  }, {ignore: ['first']});
  let keeper;
  let recovered;
  try {
    if (mode === 'same-options') {
      keeper = await watcher.subscribe(keeperRoot, () => {});
    }
    await fs.rm(root, {recursive: true});
    await waitFor(
      () => deleted.some((event) => event.type === 'delete' && event.path === root),
      'root delete event',
    );
    await fs.mkdir(root);

    const options = mode === 'same-options'
      ? {ignore: ['first']}
      : {ignore: ['second']};
    const received = [];
    recovered = await watcher.subscribe(root, (error, events) => {
      if (error) throw error;
      received.push(...(events || []));
    }, options);
    const marker = path.join(root, 'recovered-marker');
    await fs.writeFile(marker, 'ready');
    await waitFor(
      () => received.some((event) => event.type === 'create' && event.path === marker),
      'event from recovered subscription',
    );
  } finally {
    if (recovered) await recovered.unsubscribe();
    await first.unsubscribe();
    if (keeper) await keeper.unsubscribe();
    await fs.rm(parent, {recursive: true, force: true});
  }
}

main().then(
  () => console.log('macOS root recovery ok'),
  (error) => {
    console.error(error);
    process.exitCode = 1;
  },
);

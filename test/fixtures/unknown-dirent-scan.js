'use strict';

const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const watcher = require('../..');

async function main() {
  const directory = await fs.mkdtemp(
    path.join(await fs.realpath(os.tmpdir()), 'native-watcher-dt-unknown-'),
  );
  const nested = path.join(directory, 'nested');
  await fs.mkdir(nested);

  let resolveEvent;
  const observed = new Promise((resolve, reject) => {
    const timer = setTimeout(
      () => reject(new Error('nested directory was not watched')),
      3000,
    );
    resolveEvent = () => {
      clearTimeout(timer);
      resolve();
    };
  });
  const subscription = await watcher.subscribe(directory, (error, events) => {
    if (error) throw error;
    if (events.some((event) => event.path === path.join(nested, 'event'))) {
      resolveEvent();
    }
  });

  try {
    await fs.writeFile(path.join(nested, 'event'), 'event');
    await observed;
  } finally {
    await subscription.unsubscribe();
    await fs.rm(directory, {recursive: true, force: true});
  }
}

main().then(
  () => console.log('DT_UNKNOWN directory watched'),
  (error) => {
    console.error(error);
    process.exitCode = 1;
  },
);

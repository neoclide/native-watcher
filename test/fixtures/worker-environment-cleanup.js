'use strict';

const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const {Worker} = require('node:worker_threads');

const packageRoot = path.resolve(__dirname, '..', '..');
const watcher = require(packageRoot);

async function main() {
  const tempRoot = await fs.realpath(os.tmpdir());
  const directory = await fs.mkdtemp(
    path.join(tempRoot, 'native-watcher-worker-cleanup-'),
  );
  const received = new Set();
  const waiters = new Set();
  const subscription = await watcher.subscribe(directory, (error, events) => {
    if (error) throw error;
    for (const event of events) received.add(event.path);
    for (const waiter of waiters) waiter();
  });

  try {
    const worker = new Worker(
      `
        const {parentPort, workerData} = require('node:worker_threads');
        require(workerData.packageRoot)
          .subscribe(workerData.directory, () => {})
          .then(() => parentPort.postMessage('ready'));
      `,
      {eval: true, workerData: {packageRoot, directory}},
    );
    await new Promise((resolve, reject) => {
      worker.once('message', resolve);
      worker.once('error', reject);
    });
    await worker.terminate();

    for (let index = 0; index < 3; index++) {
      const eventPath = path.join(directory, `after-worker-${index}`);
      const observed = new Promise((resolve, reject) => {
        const timer = setTimeout(() => {
          waiters.delete(check);
          reject(new Error(`timed out waiting for ${eventPath}`));
        }, 3000);
        const check = () => {
          if (received.has(eventPath)) {
            clearTimeout(timer);
            waiters.delete(check);
            resolve();
          }
        };
        waiters.add(check);
      });
      await fs.writeFile(eventPath, 'event');
      await observed;
    }
  } finally {
    await subscription.unsubscribe();
    await fs.rm(directory, {recursive: true, force: true});
  }
}

main().then(
  () => console.log('worker cleanup ok'),
  (error) => {
    console.error(error);
    process.exitCode = 1;
  },
);

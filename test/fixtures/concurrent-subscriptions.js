'use strict';

const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const {Worker} = require('node:worker_threads');
const watcher = require('../..');

function runWorker(directory) {
  return new Promise((resolve, reject) => {
    const worker = new Worker(`
      const {parentPort, workerData} = require('node:worker_threads');
      const fs = require('node:fs/promises');
      const path = require('node:path');
      const watcher = require(workerData.packageRoot);
      parentPort.once('message', async () => {
        let retiredDeliveries = 0;
        for (let index = 0; index < 20; index++) {
          let active = true;
          const subscription = await watcher.subscribe(
            workerData.directory,
            () => {
              if (!active) retiredDeliveries++;
            },
          );
          await subscription.unsubscribe();
          active = false;
        }

        for (let index = 0; index < 2; index++) {
          const eventPath = path.join(
            workerData.directory,
            \`worker-\${workerData.workerId}-marker-\${index}\`,
          );
          let resolveEvent;
          const observed = new Promise((resolve) => {
            resolveEvent = resolve;
          });
          const verifier = await watcher.subscribe(
            workerData.directory,
            (error, events) => {
              if (error) throw error;
              if (events.some((event) => event.path === eventPath)) {
                resolveEvent();
              }
            },
          );
          await fs.writeFile(eventPath, 'marker');
          await observed;
          await verifier.unsubscribe();
        }
        await new Promise(setImmediate);
        if (retiredDeliveries !== 0) {
          throw new Error(\`retired callbacks received \${retiredDeliveries} events\`);
        }
        parentPort.postMessage('done');
      });
      parentPort.postMessage('ready');
    `, {
      eval: true,
      workerData: {
        directory,
        packageRoot: path.resolve(__dirname, '..', '..'),
        workerId: runWorker.nextId++,
      },
    });
    worker.once('error', reject);
    worker.once('message', () => {
      worker.once('message', resolve);
      worker.postMessage('start');
    });
  });
}

runWorker.nextId = 0;

async function main() {
  const directory = await fs.mkdtemp(
    path.join(await fs.realpath(os.tmpdir()), 'native-watcher-concurrent-'),
  );
  try {
    await Promise.all(Array.from({length: 4}, () => runWorker(directory)));
    const eventPath = path.join(directory, 'after-concurrency');
    let resolveEvent;
    const observed = new Promise((resolve, reject) => {
      const timer = setTimeout(
        () => reject(new Error('final subscription received no event')),
        3000,
      );
      resolveEvent = () => {
        clearTimeout(timer);
        resolve();
      };
    });
    const subscription = await watcher.subscribe(directory, (error, events) => {
      if (error) throw error;
      if (events.some((event) => event.path === eventPath)) resolveEvent();
    });
    try {
      await fs.writeFile(eventPath, 'event');
      await observed;
    } finally {
      await subscription.unsubscribe();
    }
  } finally {
    await fs.rm(directory, {recursive: true, force: true});
  }
}

main().then(
  () => console.log('concurrent subscriptions ok'),
  (error) => {
    console.error(error);
    process.exitCode = 1;
  },
);

'use strict';

const fs = require('node:fs');
const path = require('node:path');
const {performance} = require('node:perf_hooks');
const watcher = require('..');

const directory = process.argv[2] && path.resolve(process.argv[2]);
const iterations = Number.parseInt(process.argv[3] ?? '7', 10);

if (!directory || !fs.statSync(directory).isDirectory()) {
  console.error('Usage: npm run benchmark:subscribe -- DIRECTORY [ITERATIONS]');
  process.exit(1);
}
if (!Number.isInteger(iterations) || iterations < 1) {
  console.error('ITERATIONS must be a positive integer');
  process.exit(1);
}

async function sample() {
  if (global.gc) global.gc();
  const rssBefore = process.memoryUsage().rss;
  let maxTimerLag = 0;
  let expected = performance.now() + 1;
  const timer = setInterval(() => {
    const now = performance.now();
    maxTimerLag = Math.max(maxTimerLag, now - expected);
    expected = now + 1;
  }, 1);

  const started = performance.now();
  const subscription = await watcher.subscribe(directory, (error) => {
    if (error) throw error;
  });
  const elapsed = performance.now() - started;
  const rssDelta = process.memoryUsage().rss - rssBefore;
  clearInterval(timer);
  await subscription.unsubscribe();

  return {elapsed, maxTimerLag, rssDelta};
}

async function main() {
  const samples = [];
  for (let i = 0; i < iterations; i++) samples.push(await sample());

  const elapsed = samples.map((item) => item.elapsed).sort((a, b) => a - b);
  const middle = Math.floor(elapsed.length / 2);
  const median =
    elapsed.length % 2 === 0
      ? (elapsed[middle - 1] + elapsed[middle]) / 2
      : elapsed[middle];

  console.log(
    JSON.stringify(
      {
        directory,
        iterations,
        subscribeMs: {
          median: Number(median.toFixed(2)),
          min: Number(elapsed[0].toFixed(2)),
          max: Number(elapsed.at(-1).toFixed(2)),
          samples: samples.map((item) => Number(item.elapsed.toFixed(2))),
        },
        maxTimerLagMs: Number(
          Math.max(...samples.map((item) => item.maxTimerLag)).toFixed(2),
        ),
        firstRssDeltaMiB: Number(
          (samples[0].rssDelta / 1024 / 1024).toFixed(2),
        ),
      },
      null,
      2,
    ),
  );
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});

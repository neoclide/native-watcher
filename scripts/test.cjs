'use strict';

const {readdirSync} = require('node:fs');
const path = require('node:path');
const {spawnSync} = require('node:child_process');

// Node 20 on Windows does not expand shell globs. Pass each test explicitly
// and keep helpers and executable fixtures out of automatic test discovery.
const directory = path.join(__dirname, '..', 'test');
const files = readdirSync(directory)
  .filter((name) => name.endsWith('.test.js'))
  .sort()
  .map((name) => path.join(directory, name));
const result = spawnSync(process.execPath, [
  '--test', ...process.argv.slice(2), ...files,
], {stdio: 'inherit'});
if (result.error) throw result.error;
process.exit(result.status ?? 1);

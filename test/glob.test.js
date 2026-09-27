'use strict';

const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const {execFile} = require('node:child_process');
const {promisify} = require('node:util');

const execFileAsync = promisify(execFile);

test('matches restricted raw globs by complete path components',
  {skip: process.platform === 'win32'}, async () => {
  const directory = await fs.mkdtemp(path.join(
    await fs.realpath(os.tmpdir()), 'native-watcher-glob-',
  ));
  try {
    const sourceRoot = path.join(__dirname, '..', 'src');
    const binary = path.join(directory, 'glob');
    const platformArgs = process.platform === 'darwin'
      ? ['-DFS_EVENTS', '-framework', 'CoreServices'] : ['-DINOTIFY'];
    await execFileAsync('c++', [
      '-std=c++17', ...platformArgs,
      `-I${sourceRoot}`,
      path.join(__dirname, 'fixtures', 'glob.cc'),
      path.join(sourceRoot, 'Glob.cc'),
      '-o', binary,
    ]);
    await execFileAsync(binary);
  } finally {
    await fs.rm(directory, {recursive: true, force: true});
  }
});

test('uses macOS case folding and canonical normalization only on matching volumes',
  {skip: process.platform !== 'darwin'}, async (t) => {
    const directory = await fs.mkdtemp(path.join(
      await fs.realpath(os.tmpdir()), 'native-watcher-glob-case-',
    ));
    try {
      const sourceRoot = path.join(__dirname, '..', 'src');
      const binary = path.join(directory, 'glob-macos-case');
      await execFileAsync('c++', [
        '-std=c++17', '-DFS_EVENTS', '-framework', 'CoreServices',
        `-I${sourceRoot}`,
        path.join(__dirname, 'fixtures', 'glob-macos-case.cc'),
        path.join(sourceRoot, 'Glob.cc'),
        path.join(sourceRoot, 'CaseSensitivity.cc'),
        '-o', binary,
      ]);
      const {stdout} = await execFileAsync(binary, [directory]);
      if (stdout.includes('SKIP')) t.skip('temporary directory is case-sensitive');
    } finally {
      await fs.rm(directory, {recursive: true, force: true});
    }
  });

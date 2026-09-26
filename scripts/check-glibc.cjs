'use strict';

const {execFileSync} = require('node:child_process');

const file = process.argv[2] || 'build/Release/native_watcher.node';
const symbols = execFileSync('objdump', ['-T', file], {encoding: 'utf8'});
const versions = [...symbols.matchAll(/\bGLIBC_(\d+)\.(\d+)(?:\.(\d+))?\b/g)]
  .map((match) => match.slice(1).map((part) => Number(part || 0)))
  .sort((a, b) => a[0] - b[0] || a[1] - b[1] || a[2] - b[2]);
if (!versions.length) throw new Error(`No GLIBC symbol versions found in ${file}`);

const maximum = versions[versions.length - 1];
const version = maximum.slice(0, maximum[2] ? 3 : 2).join('.');
if (maximum[0] > 2 || (maximum[0] === 2 &&
    (maximum[1] > 28 || (maximum[1] === 28 && maximum[2] > 0)))) {
  throw new Error(`${file} requires glibc ${version}; maximum allowed is 2.28`);
}
console.log(`${file}: glibc ${version} (maximum allowed: 2.28)`);

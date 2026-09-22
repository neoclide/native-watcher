'use strict';

const path = require('path');
const {createWrapper} = require('./wrapper');

function loadBinding() {
  const candidates = [
    path.join(__dirname, 'build', 'Release', 'native_watcher.node'),
    path.join(__dirname, 'build', 'Debug', 'native_watcher.node'),
  ];

  let lastError;
  for (const candidate of candidates) {
    try {
      return require(candidate);
    } catch (error) {
      if (error.code !== 'MODULE_NOT_FOUND') throw error;
      lastError = error;
    }
  }

  const error = new Error(
    'native-watcher has not been built. Run `npm run build` in the package directory.',
  );
  error.cause = lastError;
  throw error;
}

module.exports = createWrapper(loadBinding());

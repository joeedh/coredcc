// Emscripten --pre-js for tests under node: the C environment (getenv) starts empty, so
// copy the process environment into it before main runs.
Module['preRun'] = Module['preRun'] || [];
Module['preRun'].push(function () {
  for (var k in process.env) {
    ENV[k] = process.env[k];
  }
});

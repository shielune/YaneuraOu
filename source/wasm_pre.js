// Leading semicolon: when concatenated with the emscripten-generated
// preamble (em++ injects this file via --pre-js), the previous line ends
// without a semicolon and ASI does not kick in before `(function(){…})()`,
// so the IIFE gets parsed as a call on the prior expression. With
// `EM_PTHREAD=1` that prior expression is `… == "em-pthread"`, which then
// errors with `"em-pthread" is not a function` and breaks pthread spawn.
;(function () {
  // Message listeners

  var quit = false;
  var listeners = [];

  Module["print"] = function (line) {
    if (listeners.length === 0) console.log(line);
    else
      setTimeout(function () {
        for (var i = 0; i < listeners.length; i++) listeners[i](line);
      });
  };

  Module["addMessageListener"] = function (listener) {
    listeners.push(listener);
  };

  Module["removeMessageListener"] = function (listener) {
    var idx = listeners.indexOf(listener);
    if (idx >= 0) listeners.splice(idx, 1);
  };

  Module["terminate"] = function () {
    quit = true;
    if (typeof PThread !== "undefined" && PThread.terminateAllThreads) {
      PThread.terminateAllThreads();
    }
  };

  // Command queue

  var queue = [];
  var backoff = 1;

  function poll() {
    var command = queue.shift();
    if (quit || command === undefined) return;

    if (command === "quit") return Module["terminate"]();

    var tryLater = Module["ccall"](
      "usi_command",
      "number",
      ["string"],
      [command]
    );
    if (tryLater) queue.unshift(command);
    backoff = tryLater ? backoff * 2 : 1;
    setTimeout(poll, backoff);
  }

  Module["postMessage"] = function (command) {
    queue.push(command);
  };

  Module["postRun"] = function () {
    Module["postMessage"] = function (command) {
      queue.push(command);
      if (queue.length === 1) poll();
    };
    poll();
  };
})();

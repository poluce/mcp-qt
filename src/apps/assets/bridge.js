(function () {
  function debug(level, args) {
    try {
      var message = Array.prototype.slice.call(args).join(' ');
      parent.postMessage({
        jsonrpc: '2.0',
        method: 'debug/console',
        params: { level: level, msg: String(message).slice(0, 3000) }
      }, '*');
    } catch (_) {}
  }

  window.addEventListener('error', function (event) {
    var target = event.target || {};
    debug('onerror', [event.message || 'resource load failed', target.src || target.href || '']);
  }, true);
  window.addEventListener('unhandledrejection', function (event) {
    debug('reject', [event.reason && event.reason.stack || event.reason]);
  });

  try {
    ['log', 'info', 'warn', 'error'].forEach(function (name) {
      var original = console[name];
      console[name] = function () {
        debug(name, arguments);
        original.apply(console, arguments);
      };
    });
  } catch (_) {}

  if (!window.mcpAppHost) {
    window.mcpAppHost = {
      _listeners: [],
      postMessage: function (message) {
        try { parent.postMessage(message, '*'); } catch (_) {}
      },
      addEventListener: function (listener) { this._listeners.push(listener); },
      _dispatch: function (message) {
        this._listeners.forEach(function (listener) {
          try { listener(message); } catch (_) {}
        });
      }
    };
    window.addEventListener('message', function (event) {
      window.mcpAppHost._dispatch(event.data);
    });
  }
})();

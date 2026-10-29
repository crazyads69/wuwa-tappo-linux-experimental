// Shared Linux window-coordinate bridge for the recovered frontend.
(() => {
  const I = window.__TAURI_INTERNALS__;
  if (!I) return;
  const label = I.metadata.currentWindow.label;
  const invoke = I.invoke.bind(I);
  // GTK reports (0,0) for Wayland layer windows; the frontend needs their selected output's origin.
  window.__wwInvoke = async (cmd, args = {}, options) => {
    if (cmd === 'plugin:window|monitor_from_point') {
      return invoke('monitor_from_physical_point', args, options);
    }
    if ((args.label ?? label) === 'main' && ['inner_position', 'outer_position', 'current_monitor', 'scale_factor'].some(name => cmd === `plugin:window|${name}`)) {
      try {
        const snapshot = await invoke('get_main_monitor_snapshot');
        if (snapshot?.layerShell) {
          if (cmd.endsWith('current_monitor')) return snapshot.monitor;
          if (cmd.endsWith('scale_factor')) return snapshot.monitor.scaleFactor;
          return snapshot.origin;
        }
      } catch (_) {}
    }
    return invoke(cmd, args, options);
  };
})();

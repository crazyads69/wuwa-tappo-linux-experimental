// WebKitGTK keeps no cookies for the tauri:// scheme, but the app keeps its session (token, last login time)
// in document.cookie. Back it with localStorage, which all windows share. Injected into every window.
(() => {
  if (location.protocol !== 'tauri:') return;
  const K = '__cookie_jar';
  const load = () => { try { return JSON.parse(localStorage.getItem(K) || '{}'); } catch { return {}; } };
  Object.defineProperty(document, 'cookie', {
    configurable: true,
    get() {
      const j = load(), n = Date.now();
      return Object.entries(j).filter(([, v]) => v.e > n).map(([k, v]) => `${k}=${v.v}`).join('; ');
    },
    set(s) {
      const p = String(s).split(';').map((x) => x.trim());
      const i = p[0].indexOf('=');
      if (i < 1) return;
      const k = p[0].slice(0, i), v = p[0].slice(i + 1);
      let e = Date.now() + 3.15e10; // session cookie -> ~1 year (the app always sets expires)
      for (const a of p.slice(1)) {
        if (/^expires=/i.test(a)) { const t = Date.parse(a.slice(8)); if (!isNaN(t)) e = t; }
        if (/^max-age=/i.test(a)) e = Date.now() + 1000 * Number(a.slice(8));
      }
      const j = load();
      if (e <= Date.now()) delete j[k]; else j[k] = { v, e };
      localStorage.setItem(K, JSON.stringify(j));
    },
  });
})();

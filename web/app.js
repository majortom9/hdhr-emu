const state = { scanning: {} };
const ATSC_RF_MIN = 2, ATSC_RF_MAX = 36;

function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

async function loadDeviceInfo() {
  try {
    const d = await (await fetch('/discover.json')).json();
    document.getElementById('device-name').textContent = d.FriendlyName;
    document.getElementById('device-sub').textContent =
      `${d.ModelNumber} · Device ID ${d.DeviceID} · ${d.TunerCount} tuner(s)`;
  } catch (e) { /* transient -- next poll cycle isn't scheduled for this, just leave the default title */ }
}

function ensureTunerCard(idx) {
  let card = document.querySelector(`.tuner-card[data-idx="${idx}"]`);
  if (card) return card;
  const tpl = document.getElementById('tuner-card-template');
  card = tpl.content.firstElementChild.cloneNode(true);
  card.dataset.idx = idx;
  card.querySelector('.idx').textContent = idx;
  card.querySelector('.tune-btn').addEventListener('click', () => onTune(idx));
  card.querySelector('.scan-btn').addEventListener('click', () => onScanStart(idx));
  card.querySelector('.scan-stop-btn').addEventListener('click', () => onScanStop(idx));
  card.querySelector('.release-btn').addEventListener('click', () => onRelease(idx));
  document.getElementById('tuners').appendChild(card);
  return card;
}

function pctClass(pct) {
  if (pct >= 70) return 'good';
  if (pct >= 40) return 'ok';
  return '';
}

function setBar(card, cls, pct) {
  pct = Math.max(0, Math.min(100, pct || 0));
  const fill = card.querySelector('.bar-fill.' + cls);
  fill.style.width = pct + '%';
  fill.classList.remove('ok', 'good');
  const c = pctClass(pct);
  if (c) fill.classList.add(c);
  card.querySelector('.' + cls + '-pct').textContent = pct + '%';
}

function renderFound(el, streaminfo) {
  if (!streaminfo || !streaminfo.length) { el.innerHTML = ''; return; }
  el.innerHTML = streaminfo.map(c =>
    `<div class="found-item"><span class="num">${c.major}.${c.minor}</span><span>${escapeHtml(c.name)}</span></div>`
  ).join('');
}

function renderTuner(t) {
  const card = ensureTunerCard(t.index);
  if (state.scanning[t.index]) return; /* the scan loop owns this card's display while active */

  card.querySelector('.vchannel').textContent = (t.vchannel && t.vchannel !== 'none') ? t.vchannel : '—';
  card.querySelector('.phys').textContent = `${t.physical_channel} · lock: ${t.lock}`;

  setBar(card, 'ss', t.signal_strength_pct);
  setBar(card, 'snq', t.signal_quality_pct);
  setBar(card, 'seq', t.symbol_quality_pct);

  card.querySelector('.rate').textContent = (t.bps > 0) ? `${(t.bps / 1e6).toFixed(2)} Mbps` : '';

  renderFound(card.querySelector('.found-list'), t.streaminfo);
}

async function pollTuners() {
  try {
    const d = await (await fetch('/api/tuners.json')).json();
    d.tuners.forEach(renderTuner);
  } catch (e) { /* transient network hiccup -- next tick tries again */ }
}

async function setChannel(idx, value) {
  const r = await fetch(`/api/tuner${idx}/channel`, { method: 'POST', body: value });
  return r.json();
}

async function onTune(idx) {
  const card = document.querySelector(`.tuner-card[data-idx="${idx}"]`);
  const map = card.querySelector('.channelmap').value;
  const num = card.querySelector('.chnum').value;
  if (!num) return;
  await setChannel(idx, `${map}:${num}`);
  pollTuners();
}

/* "none" is a real, native value for /tunerN/channel (see control.c's
 * handle_tuner_set) -- releasing a tuner this way stops any push,
 * closes its held frontend fd, and clears its selection, same as real
 * HDHomeRun firmware. Lets a tuner sit idle (no held-stats polling,
 * frontend free for something else to use) instead of staying parked
 * on whatever it was last tuned to. */
async function onRelease(idx) {
  if (state.scanning[idx]) onScanStop(idx);
  await setChannel(idx, 'none');
  pollTuners();
}

/* Mirrors hdhomerun_config's own manual-scan approach (and this
 * project's existing CLI-driven scan, already validated server-side):
 * walk every US ATSC RF channel sequentially, one SET at a time. No
 * separate polling needed per step -- the daemon's /tunerN/channel SET
 * already waits for the lock result (up to ~1.8s) before replying, and
 * our POST /api/tuner<N>/channel endpoint returns that channel's full
 * post-tune status (incl. streaminfo) in the same response. */
async function onScanStart(idx) {
  if (state.scanning[idx]) return;
  state.scanning[idx] = true;
  const card = document.querySelector(`.tuner-card[data-idx="${idx}"]`);
  card.querySelector('.scan-btn').hidden = true;
  card.querySelector('.scan-stop-btn').hidden = false;
  const statusEl = card.querySelector('.scan-status');
  const foundEl = card.querySelector('.found-list');
  const allFound = [];

  for (let ch = ATSC_RF_MIN; ch <= ATSC_RF_MAX; ch++) {
    if (!state.scanning[idx]) break;
    statusEl.textContent = `Scanning RF ${ch}/${ATSC_RF_MAX}...`;
    try {
      const result = await setChannel(idx, `us-bcast:${ch}`);
      if (result.lock && result.lock !== 'none' && result.streaminfo && result.streaminfo.length) {
        allFound.push(...result.streaminfo);
        renderFound(foundEl, allFound);
      }
      card.querySelector('.phys').textContent = `${result.physical_channel} · lock: ${result.lock}`;
      setBar(card, 'ss', result.signal_strength_pct);
      setBar(card, 'snq', result.signal_quality_pct);
      setBar(card, 'seq', result.symbol_quality_pct);
    } catch (e) { /* one bad channel shouldn't abort the whole scan */ }
  }

  const finished = state.scanning[idx];
  statusEl.textContent = finished
    ? `Scan complete — ${allFound.length} channel(s) found.`
    : 'Scan stopped.';
  state.scanning[idx] = false;
  card.querySelector('.scan-btn').hidden = false;
  card.querySelector('.scan-stop-btn').hidden = true;
}

function onScanStop(idx) {
  state.scanning[idx] = false;
}

/* This daemon is served over plain HTTP (no TLS), and the modern
 * navigator.clipboard API is restricted to secure contexts in most
 * browsers -- so copy uses the older execCommand fallback directly
 * rather than assuming clipboard.writeText() is available. */
function copyToClipboard(text) {
  const ta = document.createElement('textarea');
  ta.value = text;
  ta.style.position = 'fixed';
  ta.style.opacity = '0';
  document.body.appendChild(ta);
  ta.focus();
  ta.select();
  try { document.execCommand('copy'); } catch (e) { /* nothing more we can do */ }
  document.body.removeChild(ta);
}

/* iOS Safari reliably hands off a bare "vlc://" URL to VLC, but VLC
 * for Android doesn't register that scheme the same way -- confirmed
 * live (VLC installed on both, vlc:// worked on iPhone, silently did
 * nothing on Android). Chrome for Android instead has first-class
 * support for "intent://" URIs targeting a specific app's package
 * name directly, which is the standard way web pages deep-link into
 * an installed Android app. Desktop VLC also registers vlc://, so
 * that's the fallback for anything that isn't detected as Android.
 * Confirmed live: without an explicit type=, the intent opened VLC
 * but landed on its general browse screen instead of playing --
 * adding the MIME type this server's own stream endpoint actually
 * declares (see http_server.c's stream_channel_to_client, "video/mpeg")
 * gives Android enough to route straight to a playback handler, the
 * same way opening a downloaded .m3u file (whose own type,
 * audio/x-mpegurl, is unambiguous) already does. */
function vlcLink(url) {
  if (/Android/i.test(navigator.userAgent || '')) {
    const scheme = url.startsWith('https://') ? 'https' : 'http';
    const withoutScheme = url.replace(/^https?:\/\//, '');
    return `intent://${withoutScheme}#Intent;scheme=${scheme};type=video/mpeg;package=org.videolan.vlc;end`;
  }
  return `vlc://${url}`;
}

/* Same pattern as vlcLink() above, for mpv-android (package is.xyz.mpv,
 * the standard/most common mpv port on Android -- confirmed convention,
 * same type=video/mpeg fix applied preemptively since it's the same
 * "app opens without knowing what to play" issue vlcLink() hit). The
 * iOS mpv port's URL scheme is less standardized than VLC's, so
 * Confirmed working on Android; there's no equivalently standardized
 * mpv port/scheme on iOS, so the mpv link is Android-only -- see
 * mpvLinkOrNull() below, which hides it everywhere else rather than
 * show a link that's confirmed not to go anywhere. */
function mpvLinkOrNull(url) {
  if (!/Android/i.test(navigator.userAgent || '')) return null;
  const scheme = url.startsWith('https://') ? 'https' : 'http';
  const withoutScheme = url.replace(/^https?:\/\//, '');
  return `intent://${withoutScheme}#Intent;scheme=${scheme};type=video/mpeg;package=is.xyz.mpv;end`;
}

async function loadLineup() {
  try {
    const items = await (await fetch('/lineup.json')).json();
    const el = document.getElementById('lineup');
    el.innerHTML = items.map(c => {
      const mpvHref = mpvLinkOrNull(c.URL);
      return `<div class="lineup-item"><span class="num">${escapeHtml(c.GuideNumber)}</span><span class="name">${escapeHtml(c.GuideName)}</span>` +
        `<button class="copy-btn small" data-url="${escapeHtml(c.URL)}">copy</button>` +
        `<a href="${c.URL}.m3u">m3u</a>` +
        `<a href="${vlcLink(c.URL)}">vlc</a>` +
        (mpvHref ? `<a href="${mpvHref}">mpv</a>` : '') +
        `</div>`;
    }).join('') || '<div class="muted">No channels yet.</div>';
  } catch (e) {
    document.getElementById('lineup').innerHTML = '<div class="muted">Couldn’t load lineup.</div>';
  }
}

/* Delegated once on the container rather than re-attached after every
 * loadLineup() re-render (which replaces the whole innerHTML). */
document.getElementById('lineup').addEventListener('click', (e) => {
  const btn = e.target.closest('.copy-btn');
  if (!btn) return;
  copyToClipboard(btn.dataset.url);
  const orig = btn.textContent;
  btn.textContent = 'copied!';
  setTimeout(() => { btn.textContent = orig; }, 1200);
});

document.getElementById('lineup-refresh').addEventListener('click', loadLineup);

loadDeviceInfo();
loadLineup();
pollTuners();
setInterval(pollTuners, 2000);

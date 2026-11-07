// Spotify Pitch + Tempo: the audio is processed by the Spotify-only native DLL.
(async function pitchTempoExtension() {
    if (globalThis.__spotifyPitchTempo) return;
    globalThis.__spotifyPitchTempo = true;
    while (!globalThis.Spicetify?.Playbar?.Button || !Spicetify.PopupModal) {
        await new Promise(resolve => setTimeout(resolve, 250));
    }
    const TOKEN = "__BRIDGE_TOKEN__";
    const BASE = "http://127.0.0.1:43827";
    let status = null;
    let error = "Connecting to native audio backend…";
    let panel = null;
    let pending = null;
    let sending = false;
    let editingUntil = 0;
    // A problem must persist this long before the footer shows it; brief
    // "not applied yet" moments while dragging read as "applying" instead.
    const WARN_AFTER_MS = 1500;
    let problemSince = 0;
    let lastGood = null; // last confirmed speed/pitch/latency, kept on screen while applying
    const defaults = () => ({ tempo: 1000, pitch: 0, linked: 1, enabled: 0, reverb: 0, distortion: 0, muffle: 0, bass: 0, pan: 0, panSpeed: 40, spatial: 0 });
    const effectNames = ["reverb", "distortion", "muffle", "bass", "pan", "spatial"];
    const fromStatus = s => Object.fromEntries(Object.keys(defaults()).map(k => [k, s[k]]));
    const lapSeconds = speed => 20 * Math.pow(0.2, speed / 100);
    let draft = defaults();
    // Last sliders survive a Spotify restart; Enable always comes back off.
    const STORE = "spotify-pitch-tempo:settings";
    let restored = false;
    function save(settings) {
        try { localStorage.setItem(STORE, JSON.stringify({ ...settings, enabled: 0 })); } catch {}
    }
    function load() {
        try {
            const saved = JSON.parse(localStorage.getItem(STORE) || "null");
            if (!saved) return null;
            const base = defaults();
            for (const key of Object.keys(base)) if (Number.isFinite(saved[key])) base[key] = saved[key];
            return { ...base, enabled: 0 };
        } catch { return null; }
    }
    const isDefault = s => Object.entries(defaults()).every(([k, v]) => s[k] === v);
    const signed = n => `${n >= 0 ? "+" : ""}${n.toFixed(2)}`;
    async function request(path, settings) {
        const response = await fetch(BASE + path, {
            method: settings ? "POST" : "GET",
            headers: { "X-SPT-Token": TOKEN, ...(settings ? { "Content-Type": "application/x-www-form-urlencoded" } : {}) },
            body: settings ? new URLSearchParams(settings).toString() : undefined,
            signal: AbortSignal.timeout(2000),
        });
        const data = await response.json();
        if (!response.ok) throw new Error(data.error || `Native backend: ${response.status}`);
        if (data.protocol !== 4) throw new Error("Update the native helper and restart Spotify to use the FX controls.");
        return data;
    }
    function controls() { return panel ? Object.fromEntries([...panel.querySelectorAll("[data-id]")].map(e => [e.dataset.id, e])) : {}; }
    // Green fill from the slider's origin (left, or centre for bipolar sliders) to the thumb.
    function fill(input) {
        const min = Number(input.min), max = Number(input.max);
        const p = (Number(input.value) - min) / (max - min) * 100;
        const origin = input.dataset.bipolar ? 50 : 0;
        input.style.setProperty("--a", `${Math.min(origin, p)}%`);
        input.style.setProperty("--b", `${Math.max(origin, p)}%`);
    }
    function setValue(output, text, on) {
        if (output.textContent !== text) output.textContent = text;
        output.dataset.on = on ? "1" : "0";
    }
    // One fixed-height footer: a state line, three stats and one note line.
    // Text changes in place; the layout never grows or shrinks.
    function footer() {
        if (!status) return { kind: "warn", text: "Not connected", note: `${error} Start “Spotify + Pitch & Tempo” from your desktop.`, stats: null };
        const current = status.commandSequence === status.appliedSequence;
        const waiting = !current || status.transitioning;
        const applied = current && status.allApplied && !status.transitioning && !status.faulted;
        const problem = status.capacityReached ? "Stream limit reached · restart Spotify using the shortcut" :
            status.faulted ? "Audio backend fault · restart Spotify using the shortcut" :
            status.lastError ? `Native error 0x${(status.lastError >>> 0).toString(16)}` :
            status.active && !waiting && !applied && status.enabled ? "Request not fully applied · check audio support" : "";
        problemSince = problem ? (problemSince || Date.now()) : 0;
        const showProblem = problem && Date.now() - problemSince > WARN_AFTER_MS;
        if (status.active && applied) {
            const latency = status.sampleRate && status.appliedTempo ? Math.round(1000000 * status.dspLatency / (status.sampleRate * status.appliedTempo)) : 0;
            lastGood = { speed: `${(status.appliedTempo / 1000).toFixed(3)}×`, pitch: `${signed(status.appliedPitch / 100)} st`, latency: latency ? `~${latency} ms` : "None" };
        }
        if (!status.active) lastGood = null;
        const held = applied && status.enabled && status.appliedTempo < status.tempo - 0.5;
        const note = showProblem ? problem :
            held ? `Speed held at ${(status.appliedTempo / 1000).toFixed(2)}× · Spotify can't supply audio faster · pitch kept` :
            !status.streams ? "If music is already playing, quit Spotify and reopen it with the shortcut." : "";
        if (showProblem) return { kind: "warn", text: "Needs attention", note, stats: lastGood };
        if (!status.streams) return { kind: "idle", text: "Press play", note, stats: null };
        if (!status.active) return { kind: "idle", text: "Paused · settings apply when audio resumes", note, stats: null };
        if (waiting || problem) return { kind: "busy", text: "Applying…", note, stats: lastGood };
        if (!status.enabled) return { kind: "idle", text: status.bypassConfirmed ? "Bypassed · original audio" : "Bypassing…", note, stats: lastGood };
        return { kind: "ok", text: "Processing Spotify audio", note, stats: lastGood };
    }
    function draw() {
        const current = status?.commandSequence === status?.appliedSequence;
        const applied = Boolean(current && status?.allApplied && !status?.transitioning && !status?.faulted);
        button.active = Boolean(applied && status?.enabled);
        button.label = applied && status?.enabled ? `Pitch, Tempo & FX · ${(status.appliedTempo / 1000).toFixed(2)}×` : "Pitch, Tempo & FX";
        if (!panel?.isConnected) return;
        const el = controls();
        const offline = !status;
        const semitones = draft.linked ? 12 * Math.log2(draft.tempo / 1000) : draft.pitch / 100;
        setValue(el.tempoValue, `${(draft.tempo / 1000).toFixed(2)}×`, draft.tempo !== 1000);
        setValue(el.pitchValue, `${signed(semitones)} st`, Math.abs(semitones) > 0.004);
        el.tempo.value = draft.tempo;
        el.pitch.value = draft.pitch;
        el.linked.checked = Boolean(draft.linked);
        el.enabled.checked = Boolean(draft.enabled);
        el.pitch.disabled = Boolean(draft.linked) || offline;
        el.pitchCard.dataset.locked = draft.linked ? "1" : "0";
        el.tempo.disabled = el.enabled.disabled = el.linked.disabled = offline;
        for (const name of effectNames) {
            el[name].value = draft[name];
            el[name].disabled = offline;
            setValue(el[name + "Value"], draft[name] ? `${draft[name]}%` : "Off", draft[name] > 0);
        }
        el.panSpeed.value = draft.panSpeed;
        el.panSpeed.disabled = offline;
        el.panSpeedRow.dataset.dim = draft.pan ? "0" : "1";
        setValue(el.panSpeedValue, `1 lap / ${lapSeconds(draft.panSpeed).toFixed(1)} s`, draft.pan > 0);
        for (const input of panel.querySelectorAll("input[type=range]")) fill(input);
        for (const preset of panel.querySelectorAll("[data-tempo]")) {
            preset.disabled = offline;
            preset.dataset.active = draft.linked && Number(preset.dataset.tempo) === draft.tempo ? "1" : "0";
        }
        el.mode.textContent = draft.linked ? "Speed and pitch move together." : "Tempo and pitch are adjusted independently.";
        const f = footer();
        el.footer.dataset.kind = f.kind;
        if (el.stateText.textContent !== f.text) el.stateText.textContent = f.text;
        el.statSpeed.textContent = f.stats?.speed ?? "—";
        el.statPitch.textContent = f.stats?.pitch ?? "—";
        el.statLatency.textContent = f.stats?.latency ?? "—";
        if (el.note.textContent !== f.note) { el.note.textContent = f.note; el.note.title = f.note; }
    }
    async function send(settings) {
        pending = { ...settings };
        if (sending) return;
        sending = true;
        try {
            while (pending) {
                const next = pending; pending = null;
                status = await request("/config", next); error = "";
            }
        } catch (e) { status = null; error = e.message; pending = null; }
        finally { sending = false; draw(); }
    }
    function change() {
        const el = controls();
        const wasLinked = draft.linked;
        draft = { tempo: Number(el.tempo.value), pitch: Number(el.pitch.value), linked: Number(el.linked.checked), enabled: Number(el.enabled.checked) };
        for (const name of effectNames) draft[name] = Number(el[name].value);
        draft.panSpeed = Number(el.panSpeed.value);
        if (!draft.linked && wasLinked) draft.pitch = Math.max(-1200, Math.min(1200, Math.round(1200 * Math.log2(draft.tempo / 1000) / 10) * 10));
        editingUntil = Date.now() + 800; save(draft);
        draw(); void send(draft);
    }
    const slider = (id, label, min, max, step, extra = "") =>
        `<input id="spt-${id}" data-id="${id}" type="range" min="${min}" max="${max}" step="${step}" aria-label="${label}" ${extra}>`;
    const fxRow = (id, name, hint) => `<div class="spt-fx-row">
            <label class="spt-row" for="spt-${id}"><span class="spt-name">${name}</span><output data-id="${id}Value">Off</output></label>
            ${slider(id, `${name} amount`, 0, 100, 1)}<p class="spt-hint">${hint}</p></div>`;
    const toggle = (id, text) => `<label class="spt-switch"><input data-id="${id}" type="checkbox"><span class="spt-knob" aria-hidden="true"></span><span>${text}</span></label>`;
    function open() {
        panel = document.createElement("section"); panel.className = "spt-panel";
        panel.innerHTML = `
            <div class="spt-row spt-top"><span class="spt-eyebrow">Native · Windows · Spotify only</span>${toggle("enabled", "Enable")}</div>
            <div class="spt-card">
                <label class="spt-row" for="spt-tempo"><span class="spt-title">Tempo</span><output class="spt-big" data-id="tempoValue">1.00×</output></label>
                ${slider("tempo", "Tempo", 500, 1500, 10)}
                <div class="spt-range"><span>0.50×</span><span>1.00×</span><span>1.50×</span></div>
                <div class="spt-presets"><button data-tempo="850">Slowed 0.85×</button><button data-tempo="1000">Normal 1.00×</button><button data-tempo="1250">Nightcore 1.25×</button></div>
            </div>
            <div class="spt-card" data-id="pitchCard">
                <label class="spt-row" for="spt-pitch"><span class="spt-title">Pitch</span><output class="spt-big" data-id="pitchValue">+0.00 st</output></label>
                ${slider("pitch", "Pitch in semitones", -1200, 1200, 10, 'data-bipolar="1"')}
                <div class="spt-range"><span>−12 st</span><span>Original</span><span>+12 st</span></div>
                <div class="spt-link">${toggle("linked", "Link pitch to tempo")}<span class="spt-hint" data-id="mode"></span></div>
            </div>
            <div class="spt-card">
                <div class="spt-eyebrow">Texture & space · 0% is off</div>
                ${fxRow("reverb", "Reverb", "A larger, wider and farther space as you turn it up")}
                ${fxRow("distortion", "Distortion", "4× oversampled · warm saturation to dense crunch")}
                ${fxRow("muffle", "Muffle", "Smooth low-pass · darkens everything, including the reverb")}
                ${fxRow("bass", "Bass boost", "Up to +9 dB below ~90 Hz · overall level trimmed for headroom")}
            </div>
            <div class="spt-card">
                <div class="spt-eyebrow">Headphones · 0% is off</div>
                ${fxRow("pan", "8D", "The song circles around your head")}
                <div class="spt-sub" data-id="panSpeedRow">
                    <label class="spt-row" for="spt-panSpeed"><span>Speed</span><output data-id="panSpeedValue"></output></label>
                    ${slider("panSpeed", "8D speed", 0, 100, 1)}
                </div>
                ${fxRow("spatial", "Headphone 3D", "Song as speakers in front of you · reverb surrounds you")}
            </div>
            <footer class="spt-footer" data-id="footer" role="status" aria-live="polite">
                <div class="spt-state"><span class="spt-dot" aria-hidden="true"></span><span data-id="stateText"></span></div>
                <div class="spt-stats">
                    <div><small>Speed</small><b data-id="statSpeed">—</b></div>
                    <div><small>Pitch</small><b data-id="statPitch">—</b></div>
                    <div><small>Pitch latency</small><b data-id="statLatency">—</b></div>
                </div>
                <p class="spt-note" data-id="note"></p>
                <button class="spt-reset" data-id="reset">Reset all & bypass</button>
            </footer>`;
        for (const input of panel.querySelectorAll("input")) input.addEventListener("input", change);
        panel.querySelectorAll("[data-tempo]").forEach(b => b.addEventListener("click", () => {
            draft = { ...draft, tempo: Number(b.dataset.tempo), pitch: 0, linked: 1, enabled: 1 };
            editingUntil = Date.now() + 800; save(draft); draw(); void send(draft);
        }));
        controls().reset.addEventListener("click", () => {
            draft = defaults();
            editingUntil = Date.now() + 800; save(draft); draw(); void send(draft);
        });
        Spicetify.PopupModal.display({ title: "Pitch, Tempo & FX", content: panel });
        draw();
    }
    const style = document.createElement("style");
    style.textContent = `
      .spt-panel{--spt-accent:var(--spice-button,#1ed760);--spt-sub:var(--spice-subtext,#a7a7a7);--spt-line:rgba(255,255,255,.08);--spt-track:rgba(255,255,255,.16);
        width:100%;max-width:520px;box-sizing:border-box;padding:2px 0 0;color:var(--spice-text,#fff);font-size:14px}
      .spt-row{display:flex;align-items:center;justify-content:space-between;gap:16px}
      .spt-top{margin-bottom:12px}
      .spt-eyebrow{font-size:10px;font-weight:700;letter-spacing:1.2px;text-transform:uppercase;color:var(--spt-sub)}
      .spt-card{background:rgba(255,255,255,.045);border:1px solid var(--spt-line);border-radius:14px;padding:14px 16px;margin:0 0 12px;transition:border-color .2s}
      .spt-card:hover{border-color:rgba(255,255,255,.13)}
      .spt-title{font-weight:700;font-size:15px}
      .spt-panel output{font-variant-numeric:tabular-nums;color:var(--spt-sub);transition:color .2s;white-space:nowrap}
      .spt-panel output[data-on="1"]{color:var(--spt-accent)}
      .spt-big{font-size:24px;font-weight:700;min-width:5.5ch;text-align:right}
      .spt-range{display:flex;justify-content:space-between;font-size:11px;color:var(--spt-sub);font-variant-numeric:tabular-nums}
      .spt-panel input[type=range]{-webkit-appearance:none;appearance:none;width:100%;height:4px;margin:14px 0 9px;border-radius:2px;cursor:pointer;outline:none;
        background:linear-gradient(to right,var(--spt-track) 0 var(--a,0%),var(--spt-accent) var(--a,0%) var(--b,0%),var(--spt-track) var(--b,0%) 100%)}
      .spt-panel input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:14px;height:14px;border-radius:50%;background:#fff;border:0;
        box-shadow:0 1px 4px rgba(0,0,0,.55);transition:transform .12s ease}
      .spt-panel input[type=range]:hover::-webkit-slider-thumb,.spt-panel input[type=range]:active::-webkit-slider-thumb{transform:scale(1.2)}
      .spt-panel input[type=range]:focus-visible::-webkit-slider-thumb{box-shadow:0 0 0 4px rgba(30,215,96,.35),0 1px 4px rgba(0,0,0,.55)}
      .spt-panel input:disabled{opacity:.35;cursor:default}
      .spt-presets{display:flex;gap:8px;margin-top:12px;flex-wrap:wrap}
      .spt-panel button{border:1px solid rgba(255,255,255,.18);border-radius:999px;background:transparent;color:inherit;padding:7px 13px;cursor:pointer;font-size:12px;font-weight:600;
        transition:background .15s,border-color .15s,color .15s}
      .spt-panel button:hover:not(:disabled){background:rgba(255,255,255,.08);border-color:rgba(255,255,255,.35)}
      .spt-panel button:focus-visible{outline:2px solid var(--spt-accent);outline-offset:2px}
      .spt-panel button[data-active="1"]{background:var(--spt-accent);border-color:var(--spt-accent);color:#000}
      .spt-panel button:disabled{opacity:.4;cursor:default}
      .spt-card[data-locked="1"] input[type=range]{opacity:.45}
      .spt-link{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-top:12px;flex-wrap:wrap}
      .spt-switch{display:inline-flex;align-items:center;gap:10px;cursor:pointer;font-weight:600;font-size:13px;user-select:none}
      .spt-switch input{position:absolute;opacity:0;width:1px;height:1px}
      .spt-knob{position:relative;width:32px;height:18px;border-radius:9px;background:rgba(255,255,255,.22);transition:background .18s;flex:none}
      .spt-knob::after{content:"";position:absolute;top:2px;left:2px;width:14px;height:14px;border-radius:50%;background:#fff;transition:transform .18s ease}
      .spt-switch input:checked+.spt-knob{background:var(--spt-accent)}
      .spt-switch input:checked+.spt-knob::after{transform:translateX(14px)}
      .spt-switch input:focus-visible+.spt-knob{box-shadow:0 0 0 3px rgba(30,215,96,.35)}
      .spt-switch input:disabled+.spt-knob{opacity:.4}
      .spt-hint{color:var(--spt-sub);font-size:11.5px;line-height:1.4;margin:0}
      .spt-fx-row{padding:12px 0 2px}
      .spt-fx-row+.spt-fx-row,.spt-sub+.spt-fx-row{border-top:1px solid var(--spt-line);margin-top:10px}
      .spt-name{font-weight:700;font-size:14px}
      .spt-sub{margin:4px 0 0 14px;padding-left:12px;border-left:2px solid var(--spt-line);font-size:12px;color:var(--spt-sub);transition:opacity .2s}
      .spt-sub[data-dim="1"]{opacity:.5}
      .spt-footer{position:sticky;bottom:0;z-index:1;margin:0 -2px;padding:12px 14px 14px;border:1px solid var(--spt-line);border-radius:14px;
        background:rgba(22,22,22,.94);backdrop-filter:blur(8px);box-shadow:0 -10px 24px rgba(0,0,0,.35)}
      .spt-state{display:flex;align-items:center;gap:8px;font-weight:700;font-size:13px;min-height:20px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
      .spt-dot{width:8px;height:8px;border-radius:50%;flex:none;background:var(--spt-sub);transition:background .2s}
      .spt-footer[data-kind="ok"] .spt-dot,.spt-footer[data-kind="busy"] .spt-dot{background:var(--spt-accent)}
      .spt-footer[data-kind="busy"] .spt-dot{animation:spt-pulse 1s ease-in-out infinite}
      .spt-footer[data-kind="warn"] .spt-dot{background:#eab66b}
      .spt-footer[data-kind="warn"] .spt-note{color:#eab66b}
      @keyframes spt-pulse{50%{opacity:.3}}
      @media (prefers-reduced-motion:reduce){.spt-footer[data-kind="busy"] .spt-dot{animation:none}.spt-panel *{transition:none!important}}
      .spt-stats{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin:10px 0 6px}
      .spt-stats div{display:flex;flex-direction:column;gap:2px;min-width:0}
      .spt-stats small{font-size:10px;letter-spacing:.6px;text-transform:uppercase;color:var(--spt-sub)}
      .spt-stats b{font-size:14px;font-variant-numeric:tabular-nums;white-space:nowrap}
      .spt-note{font-size:11.5px;line-height:16px;height:16px;margin:0 0 10px;color:var(--spt-sub);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
      .spt-reset{width:100%}`;
    document.head.appendChild(style);
    const icon = '<svg viewBox="0 0 16 16" width="16" height="16" fill="none" stroke="currentColor" stroke-width="1.5"><path d="M3 2v12M8 2v12M13 2v12"/><path d="M1 5h4M6 11h4M11 6h4" stroke-width="3"/></svg>';
    const button = new Spicetify.Playbar.Button("Pitch, Tempo & FX", icon, open);
    if (Spicetify.Menu?.Item) new Spicetify.Menu.Item("Pitch, Tempo & FX", false, open, "volume").register();
    // A local debug handle permits UI verification without manipulating Spotify's
    // native player internals. No account or track data goes to the helper.
    globalThis.SpotifyPitchTempo = { open, getStatus: () => status };
    async function refresh() {
        if (!sending) {
            try {
                const fresh = await request("/status");
                if (!sending) {
                    status = fresh; error = "";
                    if (Date.now() > editingUntil) draft = fromStatus(status);
                    // A freshly started helper reports defaults: put the saved sliders back.
                    if (!restored) {
                        restored = true;
                        const saved = load();
                        if (saved && isDefault(fromStatus(status)) && !isDefault(saved)) {
                            draft = saved; editingUntil = Date.now() + 800; void send(draft);
                        }
                    }
                }
            } catch (e) { status = null; error = e.message || "Native backend is offline."; }
            draw();
        }
        setTimeout(refresh, panel?.isConnected ? 350 : 1500);
    }
    void refresh();
})();

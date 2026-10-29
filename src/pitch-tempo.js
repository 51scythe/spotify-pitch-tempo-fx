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
    const defaults = () => ({ tempo: 1000, pitch: 0, linked: 1, enabled: 0, reverb: 0, distortion: 0, muffle: 0 });
    const effectNames = ["reverb", "distortion", "muffle"];
    const fromStatus = s => Object.fromEntries(Object.keys(defaults()).map(k => [k, s[k]]));
    let draft = defaults();
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
        if (data.protocol !== 3) throw new Error("Update the native helper and restart Spotify to use the FX controls.");
        return data;
    }
    function controls() { return panel ? Object.fromEntries([...panel.querySelectorAll("[data-id]")].map(e => [e.dataset.id, e])) : {}; }
    function draw() {
        const current = status?.commandSequence === status?.appliedSequence;
        const applied = Boolean(current && status?.allApplied && !status?.transitioning && !status?.faulted);
        button.active = Boolean(applied && status?.enabled);
        button.label = applied && status?.enabled ? `Pitch, Tempo & FX · ${(status.appliedTempo / 1000).toFixed(2)}×` : "Pitch, Tempo & FX";
        if (!panel?.isConnected) return;
        const el = controls();
        const semitones = draft.linked ? 12 * Math.log2(draft.tempo / 1000) : draft.pitch / 100;
        el.tempoValue.textContent = `${(draft.tempo / 1000).toFixed(2)}×`;
        el.pitchValue.textContent = `${signed(semitones)} st`;
        el.tempo.value = draft.tempo;
        el.pitch.value = draft.pitch;
        el.linked.checked = Boolean(draft.linked);
        el.enabled.checked = Boolean(draft.enabled);
        el.pitch.disabled = Boolean(draft.linked) || !status;
        el.tempo.disabled = !status;
        el.enabled.disabled = !status;
        el.linked.disabled = !status;
        for (const name of effectNames) {
            el[name].value = draft[name];
            el[name].disabled = !status;
            el[name + "Value"].textContent = draft[name] ? `${draft[name]}%` : "Off";
        }
        for (const preset of panel.querySelectorAll('[data-tempo]')) preset.disabled = !status;
        el.mode.textContent = draft.linked ? "Speed and pitch move together." : "Tempo and pitch are adjusted independently.";
        if (!status) {
            el.status.textContent = `${error} Start “Spotify + Pitch & Tempo” from your desktop to connect.`;
            el.status.dataset.state = "offline";
        } else {
            const waiting = !current || status.transitioning;
            const state = status.capacityReached ? "Stream limit reached · restart Spotify using the shortcut" :
                status.faulted ? "Audio backend fault · restart Spotify using the shortcut" :
                !status.streams ? "Press play · if already playing, quit Spotify and reopen using the shortcut" :
                !status.active ? "Paused · settings apply when audio resumes" :
                !status.enabled && current && status.bypassConfirmed ? "Bypassed · original audio" :
                waiting ? "Applying settings to Spotify audio…" : applied ? "Processing Spotify audio" : "Request not fully applied · check audio support";
            const latency = status.sampleRate && status.appliedTempo ? Math.round(1000000 * status.dspLatency / (status.sampleRate * status.appliedTempo)) : 0;
            let detail = status.active ? (waiting ? "Waiting for the audio callback to settle" : `Backend ${applied ? "accepted" : "reports"} ${(status.appliedTempo / 1000).toFixed(3)}× · ${signed(status.appliedPitch / 100)} st${latency ? ` · ~${latency} ms pitch latency` : ""}`) : "Native audio backend connected";
            if (applied && status.enabled) detail += `\nReverb ${status.appliedReverb}% · Distortion ${status.appliedDistortion}% · Muffle ${status.appliedMuffle}%`;
            const warning = status.lastError ? ` · error 0x${(status.lastError >>> 0).toString(16)}` : "";
            el.status.textContent = `${state}\n${detail}${warning}`;
            el.status.dataset.state = status.lastError || status.faulted || status.capacityReached || (status.active && !waiting && !applied) ? "offline" : "online";
        }
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
        editingUntil = Date.now() + 800;
        if (!draft.linked && wasLinked) draft.pitch = Math.max(-1200, Math.min(1200, Math.round(1200 * Math.log2(draft.tempo / 1000) / 10) * 10));
        draw(); void send(draft);
    }
    function open() {
        panel = document.createElement("section"); panel.className = "spt-panel";
        panel.innerHTML = `
            <div class="spt-row"><span class="spt-eyebrow">NATIVE · WINDOWS · SPOTIFY ONLY</span><label class="spt-toggle"><input data-id="enabled" type="checkbox"> Enable</label></div>
            <div class="spt-card"><label class="spt-row" for="spt-tempo"><span>Tempo</span><output data-id="tempoValue">1.00×</output></label><input id="spt-tempo" data-id="tempo" type="range" min="500" max="1500" step="10" aria-label="Tempo"><div class="spt-range"><span>0.50×</span><span>1.00×</span><span>1.50×</span></div></div>
            <div class="spt-card"><label class="spt-row" for="spt-pitch"><span>Pitch</span><output data-id="pitchValue">+0.00 st</output></label><input id="spt-pitch" data-id="pitch" type="range" min="-1200" max="1200" step="10" aria-label="Pitch in semitones"><div class="spt-range"><span>−12 st</span><span>Original</span><span>+12 st</span></div></div>
            <label class="spt-toggle"><input data-id="linked" type="checkbox"> Link pitch to tempo</label><p class="spt-hint" data-id="mode"></p>
            <div class="spt-presets"><button data-tempo="850">Slowed 0.85×</button><button data-tempo="1000">Normal 1.00×</button><button data-tempo="1250">Nightcore 1.25×</button></div>
            <div class="spt-fx">
                <div class="spt-eyebrow">TEXTURE & SPACE · 0% IS OFF</div>
                ${[
                    ["reverb", "Reverb", "Diffuse stereo space · a fuller, longer tail as you turn it up"],
                    ["distortion", "Distortion", "4× oversampled · warm saturation to dense crunch"],
                    ["muffle", "Muffle", "Smooth low-pass · darkens the whole sound, including the reverb"],
                ].map(([id, name, hint]) => `<div class="spt-fx-row"><label class="spt-row" for="spt-${id}"><span>${name} <small>Amount</small></span><output data-id="${id}Value">Off</output></label><input id="spt-${id}" data-id="${id}" type="range" min="0" max="100" step="1" aria-label="${name} amount"><p class="spt-hint">${hint}</p></div>`).join("")}
            </div>
            <div class="spt-status" data-id="status" role="status"></div>
            <button class="spt-reset" data-id="reset">Reset all & bypass</button>`;
        for (const input of panel.querySelectorAll("input")) input.addEventListener("input", change);
        panel.querySelectorAll("[data-tempo]").forEach(b => b.addEventListener("click", () => {
            draft = { ...draft, tempo: Number(b.dataset.tempo), pitch: 0, linked: 1, enabled: 1 };
            editingUntil = Date.now() + 800; draw(); void send(draft);
        }));
        controls().reset.addEventListener("click", () => {
            draft = defaults();
            editingUntil = Date.now() + 800; draw(); void send(draft);
        });
        Spicetify.PopupModal.display({ title: "Pitch, Tempo & FX", content: panel });
        draw();
    }
    const style = document.createElement("style");
    style.textContent = `
      .spt-panel{width:100%;max-width:520px;box-sizing:border-box;padding:4px 0 8px;color:var(--spice-text,#fff)}
      .spt-row{display:flex;align-items:center;justify-content:space-between;gap:16px}.spt-eyebrow{font-size:10px;letter-spacing:1.1px;color:var(--spice-subtext,#aaa)}
      .spt-toggle{display:flex;align-items:center;gap:10px;font-size:14px;cursor:pointer}.spt-panel input{accent-color:var(--spice-button,#1ed760)}
      .spt-card{background:rgba(255,255,255,.055);border:1px solid rgba(255,255,255,.07);border-radius:12px;padding:12px 16px;margin:12px 0}.spt-card label{font-weight:600}.spt-card output{font-size:23px;font-variant-numeric:tabular-nums;color:var(--spice-button,#1ed760)}
      .spt-panel input[type=range]{width:100%;margin:12px 0 6px;cursor:pointer}.spt-panel input:disabled{opacity:.4;cursor:default}.spt-range{display:flex;justify-content:space-between;font-size:11px;color:var(--spice-subtext,#aaa)}
      .spt-fx{background:rgba(255,255,255,.035);border:1px solid rgba(255,255,255,.07);border-radius:12px;padding:14px 16px;margin:16px 0 0}.spt-fx-row{padding-top:12px}.spt-fx-row+div{border-top:1px solid rgba(255,255,255,.06);margin-top:10px}.spt-fx-row label{font-weight:600;font-size:14px}.spt-fx-row small{font-weight:400;font-size:11px;color:var(--spice-subtext,#aaa);margin-left:5px}.spt-fx-row output{color:var(--spice-button,#1ed760);font-variant-numeric:tabular-nums}.spt-fx-row .spt-hint{font-size:11px;margin:0;line-height:1.4}
      .spt-hint{color:var(--spice-subtext,#aaa);font-size:12px;margin:8px 0 16px}.spt-presets{display:flex;gap:8px}.spt-panel button{border:1px solid rgba(255,255,255,.2);border-radius:20px;background:transparent;color:inherit;padding:8px 12px;cursor:pointer;font-size:12px}.spt-panel button:hover{background:rgba(255,255,255,.09);border-color:var(--spice-button,#1ed760)}
      .spt-status{white-space:pre-line;font-size:12px;line-height:1.7;margin:18px 0;color:var(--spice-subtext,#bbb)}.spt-status[data-state=offline]{color:#eab66b}.spt-reset{width:100%}`;
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
                }
            } catch (e) { status = null; error = e.message || "Native backend is offline."; }
            draw();
        }
        setTimeout(refresh, panel?.isConnected ? 350 : 1500);
    }
    void refresh();
})();

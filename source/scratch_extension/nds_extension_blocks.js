// =============================================================================
// ScratchDS NDS Extension — Scratch 3.0 Editor Plugin
// nds_extension_blocks.js
//
// Drop this file into the Scratch editor extensions system or load it via
// a custom Scratch fork (e.g. TurboWarp, or a modified scratch-vm build).
//
// This file defines the block UI and stubs for all NDS-specific blocks.
// On hardware, the actual execution is handled by the C++ VM in vm.cpp.
// In the browser editor, the blocks run simulated stubs using keyboard/mouse.
//
// Usage (TurboWarp / custom scratch-vm):
//   Place in scratch-vm/src/extensions/scratch3_nds/
//   Register in scratch-vm/src/extension-support/extension-manager.js
// =============================================================================

(function (Scratch) {
    'use strict';

    // -----------------------------------------------------------------------
    // Extension colours — NDS grey/blue hardware palette
    // -----------------------------------------------------------------------
    const COLOUR_PRIMARY   = '#2C5FA8'; // NDS button blue
    const COLOUR_SECONDARY = '#1A3D6B';
    const COLOUR_TEXT      = '#FFFFFF';

    // -----------------------------------------------------------------------
    // Button name list (maps to NDS KEY_ constants in C++)
    // -----------------------------------------------------------------------
    const BUTTONS = [
        { value: 'A',      text: 'A' },
        { value: 'B',      text: 'B' },
        { value: 'X',      text: 'X' },
        { value: 'Y',      text: 'Y' },
        { value: 'L',      text: 'L' },
        { value: 'R',      text: 'R' },
        { value: 'start',  text: 'Start' },
        { value: 'select', text: 'Select' },
        { value: 'up',     text: 'D-Up' },
        { value: 'down',   text: 'D-Down' },
        { value: 'left',   text: 'D-Left' },
        { value: 'right',  text: 'D-Right' },
    ];

    // -----------------------------------------------------------------------
    // Combo list
    // -----------------------------------------------------------------------
    const COMBOS = [
        { value: 'L+R',     text: 'L + R' },
        { value: 'A+B',     text: 'A + B' },
        { value: 'L+A',     text: 'L + A' },
        { value: 'R+A',     text: 'R + A' },
        { value: 'up+A',    text: 'D-Up + A' },
        { value: 'down+A',  text: 'D-Down + A' },
        { value: 'left+A',  text: 'D-Left + A' },
        { value: 'right+A', text: 'D-Right + A' },
    ];

    // -----------------------------------------------------------------------
    // Keyboard simulation map (for browser testing)
    // Maps NDS button names to keyboard keys
    // -----------------------------------------------------------------------
    const KEY_MAP = {
        'A': 'z', 'B': 'x', 'X': 'a', 'Y': 's',
        'L': 'q', 'R': 'w', 'start': 'Enter', 'select': 'Backspace',
        'up': 'ArrowUp', 'down': 'ArrowDown', 'left': 'ArrowLeft', 'right': 'ArrowRight',
    };

    // Track key states for browser simulation
    const heldKeys   = new Set();
    const pressedKeys = new Set(); // just-pressed this frame
    document.addEventListener('keydown', (e) => {
        if (!heldKeys.has(e.key)) pressedKeys.add(e.key);
        heldKeys.add(e.key);
    });
    document.addEventListener('keyup', (e) => {
        heldKeys.delete(e.key);
    });

    // Simple clap simulation via Web Audio API
    let micLoudness = 0;
    let micStream   = null;
    let micAnalyser = null;

    async function initMic() {
        if (micStream) return;
        try {
            const stream = await navigator.mediaDevices.getUserMedia({ audio: true });
            const ctx    = new AudioContext();
            const source = ctx.createMediaStreamSource(stream);
            micAnalyser  = ctx.createAnalyser();
            micAnalyser.fftSize = 256;
            source.connect(micAnalyser);
            micStream = stream;

            // Poll loudness
            const buf = new Uint8Array(micAnalyser.frequencyBinCount);
            setInterval(() => {
                micAnalyser.getByteFrequencyData(buf);
                let sum = 0;
                for (const v of buf) sum += v;
                micLoudness = Math.round((sum / buf.length / 255) * 100);
            }, 50);
        } catch (e) {
            console.warn('ScratchDS: Mic unavailable in browser:', e);
        }
    }

    // -----------------------------------------------------------------------
    // The Extension class
    // -----------------------------------------------------------------------
    class NDSExtension {
        constructor() {
            this._lastButtonPressed = {};
            this._touchX = 0;
            this._touchY = 0;
            this._touching = false;
            this._clapDetected = false;
            this._clapCooldown = 0;
            this._prevLoudness = 0;

            // Clear just-pressed state each Scratch tick
            setInterval(() => { pressedKeys.clear(); }, 1000 / 30);
        }

        getInfo() {
            return {
                id: 'nds',
                name: 'NDS Hardware',
                color1: COLOUR_PRIMARY,
                color2: COLOUR_SECONDARY,
                color3: COLOUR_TEXT,
                blocks: [
                    // ── Section: Triggers ──────────────────────────────────
                    {
                        opcode: 'whenButtonPressed',
                        blockType: Scratch.BlockType.HAT,
                        text: 'when [BUTTON] pressed',
                        arguments: {
                            BUTTON: { type: Scratch.ArgumentType.STRING,
                                      menu: 'buttons',
                                      defaultValue: 'A' }
                        }
                    },
                    {
                        opcode: 'whenClap',
                        blockType: Scratch.BlockType.HAT,
                        text: 'when clap detected',
                    },
                    {
                        opcode: 'whenTouched',
                        blockType: Scratch.BlockType.HAT,
                        text: 'when touchscreen tapped',
                    },
                    {
                        opcode: 'whenCombo',
                        blockType: Scratch.BlockType.HAT,
                        text: 'when combo [COMBO] held',
                        arguments: {
                            COMBO: { type: Scratch.ArgumentType.STRING,
                                     menu: 'combos',
                                     defaultValue: 'L+R' }
                        }
                    },

                    '---', // separator

                    // ── Section: Button Input ──────────────────────────────
                    {
                        opcode: 'buttonPressed',
                        blockType: Scratch.BlockType.BOOLEAN,
                        text: '[BUTTON] just pressed?',
                        arguments: {
                            BUTTON: { type: Scratch.ArgumentType.STRING,
                                      menu: 'buttons',
                                      defaultValue: 'A' }
                        }
                    },
                    {
                        opcode: 'buttonHeld',
                        blockType: Scratch.BlockType.BOOLEAN,
                        text: '[BUTTON] held?',
                        arguments: {
                            BUTTON: { type: Scratch.ArgumentType.STRING,
                                      menu: 'buttons',
                                      defaultValue: 'A' }
                        }
                    },
                    {
                        opcode: 'buttonReleased',
                        blockType: Scratch.BlockType.BOOLEAN,
                        text: '[BUTTON] released?',
                        arguments: {
                            BUTTON: { type: Scratch.ArgumentType.STRING,
                                      menu: 'buttons',
                                      defaultValue: 'A' }
                        }
                    },
                    {
                        opcode: 'comboHeld',
                        blockType: Scratch.BlockType.BOOLEAN,
                        text: 'combo [COMBO] held?',
                        arguments: {
                            COMBO: { type: Scratch.ArgumentType.STRING,
                                     menu: 'combos',
                                     defaultValue: 'L+R' }
                        }
                    },

                    '---',

                    // ── Section: Touchscreen ──────────────────────────────
                    {
                        opcode: 'touchPressed',
                        blockType: Scratch.BlockType.BOOLEAN,
                        text: 'touchscreen pressed?',
                    },
                    {
                        opcode: 'touchX',
                        blockType: Scratch.BlockType.REPORTER,
                        text: 'touch x',
                    },
                    {
                        opcode: 'touchY',
                        blockType: Scratch.BlockType.REPORTER,
                        text: 'touch y',
                    },
                    {
                        opcode: 'touchDeltaX',
                        blockType: Scratch.BlockType.REPORTER,
                        text: 'touch drag x',
                    },
                    {
                        opcode: 'touchDeltaY',
                        blockType: Scratch.BlockType.REPORTER,
                        text: 'touch drag y',
                    },

                    '---',

                    // ── Section: Microphone ───────────────────────────────
                    {
                        opcode: 'micLoudness',
                        blockType: Scratch.BlockType.REPORTER,
                        text: 'mic loudness',
                    },
                    {
                        opcode: 'clapDetected',
                        blockType: Scratch.BlockType.BOOLEAN,
                        text: 'clap detected?',
                    },
                    {
                        opcode: 'micRecording',
                        blockType: Scratch.BlockType.BOOLEAN,
                        text: 'mic is recording?',
                    },

                    '---',

                    // ── Section: System ───────────────────────────────────
                    {
                        opcode: 'setRumble',
                        blockType: Scratch.BlockType.COMMAND,
                        text: 'set rumble [STATE]',
                        arguments: {
                            STATE: { type: Scratch.ArgumentType.STRING,
                                     menu: 'onoff',
                                     defaultValue: 'on' }
                        }
                    },
                    {
                        opcode: 'pulseRumble',
                        blockType: Scratch.BlockType.COMMAND,
                        text: 'rumble for [SECS] seconds',
                        arguments: {
                            SECS: { type: Scratch.ArgumentType.NUMBER,
                                    defaultValue: 0.25 }
                        }
                    },
                    {
                        opcode: 'setTopBacklight',
                        blockType: Scratch.BlockType.COMMAND,
                        text: 'set top backlight [STATE]',
                        arguments: {
                            STATE: { type: Scratch.ArgumentType.STRING,
                                     menu: 'onoff',
                                     defaultValue: 'on' }
                        }
                    },
                    {
                        opcode: 'setBottomBacklight',
                        blockType: Scratch.BlockType.COMMAND,
                        text: 'set bottom backlight [STATE]',
                        arguments: {
                            STATE: { type: Scratch.ArgumentType.STRING,
                                     menu: 'onoff',
                                     defaultValue: 'on' }
                        }
                    },
                ],

                menus: {
                    buttons: { acceptReporters: false, items: BUTTONS },
                    combos:  { acceptReporters: false, items: COMBOS },
                    onoff:   { acceptReporters: false,
                               items: [
                                   { value: 'on',  text: 'on' },
                                   { value: 'off', text: 'off' }
                               ]},
                }
            };
        }

        // ── Hat block implementations (browser simulation) ────────────────

        whenButtonPressed({ BUTTON }) {
            const key = KEY_MAP[BUTTON] || BUTTON;
            return pressedKeys.has(key);
        }
        whenClap() {
            return this._detectClap();
        }
        whenTouched() {
            // In browser: simulate with mouse click on stage
            return false; // placeholder
        }
        whenCombo({ COMBO }) {
            const keys = COMBO.split('+').map(b => KEY_MAP[b] || b);
            return keys.every(k => heldKeys.has(k));
        }

        // ── Button reporters ──────────────────────────────────────────────

        buttonPressed({ BUTTON }) {
            const key = KEY_MAP[BUTTON] || BUTTON;
            return pressedKeys.has(key);
        }
        buttonHeld({ BUTTON }) {
            const key = KEY_MAP[BUTTON] || BUTTON;
            return heldKeys.has(key);
        }
        buttonReleased({ BUTTON }) {
            // Not reliably trackable without a per-frame delta
            return false;
        }
        comboHeld({ COMBO }) {
            const keys = COMBO.split('+').map(b => KEY_MAP[b] || b);
            return keys.every(k => heldKeys.has(k));
        }

        // ── Touchscreen reporters ─────────────────────────────────────────

        touchPressed() {
            return this._touching;
        }
        touchX() { return this._touchX; }
        touchY() { return this._touchY; }
        touchDeltaX() { return 0; } // browser stub
        touchDeltaY() { return 0; }

        // ── Microphone reporters ──────────────────────────────────────────

        micLoudness() {
            initMic();
            return micLoudness;
        }
        clapDetected() {
            return this._detectClap();
        }
        micRecording() {
            return micStream !== null;
        }

        // ── System commands ───────────────────────────────────────────────

        setRumble({ STATE }) {
            // No-op in browser
            console.log('[NDS] Rumble:', STATE);
        }
        pulseRumble({ SECS }) {
            console.log('[NDS] Rumble pulse:', SECS, 's');
        }
        setTopBacklight({ STATE }) {
            console.log('[NDS] Top backlight:', STATE);
        }
        setBottomBacklight({ STATE }) {
            console.log('[NDS] Bottom backlight:', STATE);
        }

        // ── Internal helpers ──────────────────────────────────────────────

        _detectClap() {
            const THRESHOLD = 60;
            const QUIET     = 20;
            const now = Date.now();
            if (!this._lastClapTime) this._lastClapTime = 0;
            if (!this._wasQuiet)     this._wasQuiet = true;

            const loud = micLoudness;
            if (now - this._lastClapTime < 300) return false; // cooldown

            if (this._wasQuiet && loud >= THRESHOLD) {
                this._lastClapTime = now;
                this._wasQuiet = false;
                return true;
            }
            if (loud <= QUIET) this._wasQuiet = true;
            return false;
        }
    }

    // -----------------------------------------------------------------------
    // Register the extension
    // -----------------------------------------------------------------------
    Scratch.extensions.register(new NDSExtension());

}(Scratch));

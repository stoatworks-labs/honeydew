/**
 * Honeydew — browser demo.
 *
 * A thin layer of real reagents in a Petri dish on a lightbox (SW Honeydew,
 * HD01, a source) or under the clip (SW Honeydew Over, HD02, an effect). The
 * one idea, from AGENTS.md: every reaction is a published mechanism with its
 * rate constants in mol/L and seconds, the species diffuse with real diffusion
 * coefficients across a dish whose width is in millimetres, and the colour is
 * never a palette — each species has a molar absorption spectrum, the layer's
 * transmittance is Beer–Lambert through its Depth, and the picture is the
 * lightbox's spectrum through it, integrated against the CIE 1931 observer.
 *
 * ---------------------------------------------------------------------------
 * What runs here, and what does not
 * ---------------------------------------------------------------------------
 *
 * **This page does not port the plugin. It runs it.** `honeydew-core.wasm` is
 * the plugin's own C++ compiled UNMODIFIED with emscripten
 * (demo/tools/build-wasm.sh): `source/Honeydew.cpp` — the plugin class, with
 * its constructor, its clock and seconds-or-milliseconds vote, its transport,
 * the buttons, the grid and its re-sampling, the seed, the substep plan, every
 * pass and every uniform, Clock Sync and the drops — and everything it calls:
 * Chemistry (every mechanism and constant), Spectra (every spectrum, the
 * lightboxes, the primaries, the CIE tables), Controls, BrEngine (the
 * Briggs–Rauscher mechanism in double on the CPU), Audio, PassBuffer, Shaders
 * and Diag, with the FFGL SDK's CFFGLPlugin, its parameter bookkeeping,
 * FFGLShader and FFGLScreenQuad. Its GL calls go to this page's WebGL2 context
 * through emscripten's GL library, and its GLSL is compiled by WebGL2 after the
 * kit's `port()` (the version line and the ES precision defaults).
 *
 * So the two engines the README describes both run here as the plugin runs
 * them: the GPU reactions (Belousov–Zhabotinsky as the three-variable
 * Oregonator with ROS2 per texel, CDIMA Turing as Lengyel–Epstein, the iodine
 * clock, the blue-bottle family, the chemical chameleon) in the shipped GLSL
 * under WebGL2, and Briggs–Rauscher in the shipped C++ in double, on this
 * page's thread.
 *
 * What is the page's and not the plugin's:
 *
 *   - **The host.** demo/wasm/glue.cpp constructs the plugin (what the two
 *     registration files do in a bundle), reads its parameter declarations
 *     back through the SDK's host getters — the panel below is built from
 *     them, not from a list typed here — and forwards SetFloatParameter,
 *     SetTime, SetBeatInfo and ProcessOpenGL, as tools/hdtest's Rig does.
 *   - **Six GL entry points** (demo/wasm/gl_shim.cpp). glShaderSource: the
 *     plugin hands GL desktop GLSL 4.10; `shaderSource()` below refuses any
 *     text that is not one of the programs this page's copy assembles
 *     (shaders.js, held to source/Shaders.cpp by demo/tools/check_shaders.py,
 *     with the ASSEMBLY table that mirrors InitGL), then applies the kit's
 *     `port()`. glEnable / glDisable / glIsEnabled: GL_PROGRAM_POINT_SIZE,
 *     which the plugin saves and restores every frame, does not exist in
 *     WebGL2. glTexImage2D / glGetTexImage: WebGL2 reads a texture back only
 *     through a framebuffer, so the plugin's two read-backs (the dish's mean,
 *     the Over's thumbnail) go through one.
 *   - **One thread.** The Briggs–Rauscher engine runs its cells on worker
 *     threads where the machine has cores; this WebAssembly is built without
 *     threads and emscripten's libc reports one core, so the plugin's own
 *     single-worker path runs them on the page's thread. Same result, slower
 *     frame.
 *   - **The panel's read-outs.** The number beside a slider is Controls.cpp's
 *     conversion, compiled in (glue.cpp's hd_convert pairs each control with
 *     the function Honeydew.cpp applies to it); the units printed after it,
 *     the recipe line's species names and every tooltip are this page's words.
 *   - **The presets** are this page's choices of settings, converted to the
 *     host's 0..1 by the plugin's own ParamFrom* functions.
 *
 * ---------------------------------------------------------------------------
 * Decisions this page made, and why
 * ---------------------------------------------------------------------------
 *
 * **Both plugins, one page**, as conway's and polyhedral's demos do it: the
 * Plugin dropdown is a new instance at that constructor's defaults.
 * `?plugin=over` opens the effect. Drop Position is two rows of the same name
 * because the Over's list has one more element (Brightest) and the kit's
 * dropdowns have a fixed list; only the chosen plugin's is shown.
 *
 * **Kit types for FFGL types.** The kit has no event or integer control.
 * Reset, Drop, Break Wave and Shake (FF_TYPE_EVENT) are buttons that hold the
 * press through one frame and release it on the next, as Resolume sends 1 then
 * 0 on consecutive frames — the plugin reads the rising edge inside
 * ProcessOpenGL, so a press that was released before the frame would be lost.
 * Seed (FF_TYPE_INTEGER) is a number field clamped to the declared range.
 *
 * **Nothing audio.** Audio is an FFT buffer Resolume fills; Audio Drops and
 * Audio Shakes act on its onsets. A browser has no Resolume FFT, so all three
 * are absent from this panel rather than present and dead (conway's choice):
 * with no spectrum the plugin's analyser never fires, so leaving them out
 * changes nothing the page draws.
 *
 * **Restart is a fresh dish.** The kit's Restart puts the clock back to 0,
 * which the plugin takes as a jump (no time passes); this page also presses
 * the plugin's Reset on it, so Restart does what a visitor means by it.
 *
 * **The About block is not on the panel**, as on every page in the suite.
 */

import { mountDemo } from './vendor/demo.js';
import { port, GLError } from './vendor/gl.js';
import * as S from './shaders.js';
import createHoneydew from './honeydew-core.js';

//===========================================================================
// FFGL.h's parameter types, by number: what GetParamType returns.
//===========================================================================

const FF_TYPE_BOOLEAN = 0;
const FF_TYPE_EVENT = 1;
const FF_TYPE_STANDARD = 10;
const FF_TYPE_OPTION = 11;
const FF_TYPE_BUFFER = 12;
const FF_TYPE_INTEGER = 13;
const FF_TYPE_TEXT = 100;

//===========================================================================
// The GLSL: the page's checked copy, assembled as InitGL assembles the
// plugin's (kVersion + kQuadVertex; Assemble( pieces ), which is kVersion +
// kCommon + the pieces). shaders.js's ASSEMBLY table is check_shaders.py's.
//===========================================================================

const PROGRAMS = new Map();
for (const [stage, pieces] of Object.entries(S.ASSEMBLY)) {
  PROGRAMS.set(S.VERSION + pieces.map((piece) => S[piece]).join(''), stage);
}

const shaderCheck = { refused: null, compiled: new Set() };

/**
 * Called by gl_shim.cpp's glShaderSource with the text the compiled plugin
 * handed to GL. Anything but one of this page's own assembled programs is
 * refused (an empty source, so the plugin's InitGL fails and says so) rather
 * than compiled.
 */
function shaderSource(stage, text) {
  const which = PROGRAMS.get(text);
  if (which === undefined || (which === 'vertex') !== (stage === 'vertex')) {
    shaderCheck.refused = { stage, length: text.length };
    return '';
  }
  shaderCheck.compiled.add(which);
  return port(text, { stage });
}

//===========================================================================
// The module.
//===========================================================================

const printed = [];
let core;
let liveInstance = 0;//the plugin instance on the page, for read-outs that ask it something
try {
  core = await createHoneydew({
    honeydewShaderSource: shaderSource,
    print: (text) => { printed.push(text); console.log(text); },
    printErr: (text) => { printed.push(text); console.warn(text); },
  });
} catch (error) {
  const root = document.querySelector('#demo');
  root.textContent = `The demo could not start: this browser would not run its WebAssembly (${error.message}). The plugin, its source and its downloads are at https://github.com/stoatworks-labs/honeydew.`;
  throw error;
}

const text = (pointer) => core.UTF8ToString(pointer);

//===========================================================================
// The declarations, read from each plugin's constructor through the SDK.
//===========================================================================

function declarations(effect) {
  const h = core._hd_new(effect ? 1 : 0);
  const list = [];
  const n = core._hd_param_count(h);
  for (let i = 0; i < n; i += 1) {
    const type = core._hd_param_type(h, i);
    const d = {
      index: i,
      id: core._hd_param_id(h, i),
      name: text(core._hd_param_name(h, i)),
      type,
      usage: core._hd_param_usage(h, i),
      group: text(core._hd_param_group(h, i)),
    };
    d.default = type === FF_TYPE_TEXT ? text(core._hd_param_default_text(h, i)) : core._hd_param_default(h, i);
    if (type === FF_TYPE_OPTION) {
      d.elements = [];
      d.values = [];
      for (let e = 0; e < core._hd_param_element_count(h, i); e += 1) {
        d.elements.push(text(core._hd_param_element_name(h, i, e)));
        d.values.push(core._hd_param_element_value(h, i, e));
      }
    }
    if (type === FF_TYPE_INTEGER) {
      d.min = core._hd_param_range_min(h, i);
      d.max = core._hd_param_range_max(h, i);
    }
    list.push(d);
  }
  const maxInputs = core._hd_max_inputs(h);
  core._hd_delete(h);
  return { list, maxInputs };
}

const DECLARED = { source: declarations(false), over: declarations(true) };
const VARIANTS = ['source', 'over'];

/** The plugin's own id (Controls.h's ParamId) for a control's name. */
const idOf = (name) => (DECLARED.over.list.find((d) => d.name === name) ?? DECLARED.source.list.find((d) => d.name === name))?.id;

//===========================================================================
// The read-outs: Controls.cpp's number, this page's unit.
//===========================================================================

const fixed = (n, d) => n.toFixed(d);
const UNITS = {
  Oxidant: (x) => (x <= 0 ? 'none' : `× ${fixed(x, 2)}`),
  'Acid or Base': (x) => (x <= 0 ? 'none' : `× ${fixed(x, 2)}`),
  Reductant: (x) => (x <= 0 ? 'none' : `× ${fixed(x, 2)}`),
  Indicator: (x) => (x <= 0 ? 'none (an empty dish)' : `× ${fixed(x, 2)}`),
  'Dish Width': (x) => `${fixed(x, x < 100 ? 1 : 0)} mm`,
  Depth: (x) => `${fixed(x, 2)} mm`,
  'Time-lapse': (x) => `${fixed(x, x < 10 ? 1 : 0)}×`,
  'Drop Size': (x) => `${fixed(x, 1)} mm`,
  'Auto Drop': (x) => (x <= 0 ? 'off' : `${fixed(x, x < 10 ? 1 : 0)} a minute`),
  Exposure: (x) => `${x >= 0 ? '+' : '−'}${fixed(Math.abs(x), 2)} stops`,
  // The regime is the plugin's own verdict at the sliders' recipe (Chemistry.cpp:
  // two catalyst peaks in 200 s of the ODE), not a fixed number: at the 1×
  // recipe it stops oscillating at f 1.78, at twice the acid at 2.16.
  Excitability: (x) => `f ${fixed(x, 2)}${(liveInstance ? core._hd_bz_oscillates(liveInstance, x) : core._hd_bz_oscillates_1x(x)) ? ' (oscillating)' : ' (excitable)'}`,
};

function readout(d) {
  return (v) => {
    const x = core._hd_convert(d.id, Math.fround(v));
    if (Number.isNaN(x)) return fixed(v, 2);
    return (UNITS[d.name] ?? ((y) => fixed(y, 3)))(x);
  };
}

/** Tooltips: this page's words, from the README's description of each control. */
const HINTS = {
  Reaction: 'Belousov–Zhabotinsky (the Oregonator, on the GPU), Briggs–Rauscher (De Kepper–Epstein, ten species in double on the CPU, stirred only), Iodine Clock (Harcourt–Esson), CDIMA Turing (Lengyel–Epstein), Traffic Light, Blue Bottle and Vanishing Valentine (Pons et al. 2000’s rate law with the dye as data), Chemical Chameleon (permanganate → manganate → MnO₂). Changing it is a fresh dish.',
  Catalyst: 'BZ only: Ferroin (red ⇄ blue), Ru(bpy)₃ (orange ⇄ pale green, and photosensitive: on SW Honeydew Over the clip’s light makes bromide where it is bright) or Cerium (colourless ⇄ yellow).',
  Reactor: 'Batch: the reagents run down. Flow: a stirred-tank feed at the recipe, residence 300 s (800 s for Briggs–Rauscher), so the dish oscillates for ever. The default.',
  Reset: 'A fresh dish, with new pacemakers. FF_TYPE_EVENT: this button holds the press through one frame.',
  Seed: 'The pacemakers and the drops’ positions. FF_TYPE_INTEGER, 0–9999. A new value is a fresh dish.',
  Oxidant: 'A log multiplier (¼× to 4×, 1× at the middle, none at the bottom) of the reaction’s cited recipe: bromate, iodate, H₂O₂, ClO₂, the air a Shake delivers, or KMnO₄ per dose. The recipe line under the picture gives the molarity.',
  'Acid or Base': 'The same multiplier on [H⁺] (BZ, Briggs–Rauscher, the clock, CDIMA) or [OH⁻] (the dye family, the chameleon). Trigger waves speed up with acid; the traffic light’s green moves to blue at ¼× base and yellow at 4×.',
  Reductant: 'Malonic acid, thiosulfate or glucose, by reaction.',
  Indicator: 'The catalyst, the starch, the dye or the dish’s starting permanganate. At the bottom there is none: an empty dish, which on the Over returns the clip exactly.',
  Vessel: 'Full Frame: the layer fills the picture. Petri Dish: a round dish with no-flux walls and a rim.',
  'Dish Width': 'Millimetres across the picture, 10 to 300 (geometric). The cell is Dish Width over Detail: wave speeds and the Turing wavelength are in mm, so a smaller dish magnifies them.',
  Depth: 'The layer’s depth, 0.3 to 20 mm: the colour’s path length in Beer–Lambert. A 1.5 mm layer is pale; 7 mm is a demonstration beaker’s colour.',
  Stir: 'A stir bar’s vortex and eddies (a closure, AGENTS.md); past 0.6 the whole vessel mixes and a BZ dish flips red–blue–red in unison.',
  'Time-lapse': 'Chemical seconds per real second, 1 to 300× (geometric). A frame runs at most 96 substeps; past that the chemistry falls behind and the status line counts it.',
  Detail: 'Cells across the frame: 128, 256, 512 or 1024. The grid’s rows follow the picture’s aspect. 256 is the default here as in the plugin; 1024 is a laptop GPU’s afternoon.',
  Drop: 'The reaction’s own dose: a silver spot that fires a BZ wave, thiosulfate into the clock, air into the dye family, permanganate into the chameleon, an iodide perturbation into CDIMA and Briggs–Rauscher. FF_TYPE_EVENT.',
  'Drop Size': 'The drop’s diameter, 1 to 20 mm.',
  'Drop Position': 'Random (inside the dish, by the drop’s serial), Centre; SW Honeydew Over adds Brightest, where the clip is brightest.',
  'Break Wave': 'BZ: a pipette drawn through the middle of the dish erases a wave and leaves the medium refractory, so the two ends curl into a pair of spirals — with Excitability in the excitable regime (the read-out says so; 0.7 is a good setting); at the default the next bulk firing overruns them. FF_TYPE_EVENT.',
  Shake: 'Air into the dish (the dye family) with a burst of stirring that decays over 0.7 s. FF_TYPE_EVENT.',
  'Auto Drop': 'Off, or 1 to 60 drops a minute of chemical time.',
  'Clock Sync': 'Off, Beat or Bar: the clock’s thiosulfate, the dye family’s air and the chameleon’s permanganate are sized or timed so the snap, the fade or the green lands on the next beat or bar of the transport. The page has no bar phase, so the plugin runs its own at the Host BPM below the picture.',
  'Light Coupling': 'SW Honeydew Over: how much of the clip’s brightness the photosensitive chemistries see. Ru(bpy)₃ BZ waves stop at a lit region’s edge; a CDIMA pattern prints the clip’s bright parts.',
  Lightbox: 'The source’s light: Daylight D65 (the CIE table), LED 5000K or Warm White (modelled: a 450 nm pump and a phosphor band). The Over has none: its light is the clip.',
  Exposure: '±2 stops on the picture.',
  'Seed From Clip': 'SW Honeydew Over: a fresh dish is excited where the clip is bright.',
  Mix: 'SW Honeydew Over: the filtered clip against the clip. At 0 the clip is returned bit for bit.',
  Excitability: 'BZ only (0.1.1): the Oregonator’s stoichiometric factor f, 1 to 4 (geometric). At low f the dish oscillates in bulk and a cut wave is overrun; past the model’s own boundary (f 1.78 at the 1× recipe, slider 0.42, moving with the acid: the read-out says which) the layer is excitable, quiet until a Drop, and a Break Wave winds a pair of spirals. The default 1.4 is the 0.1.0 dish.',
};

//===========================================================================
// Kit parameters from the declarations. The id is the name as Arena
// addresses it (lower case, spaces removed).
//===========================================================================

const KIT_TYPES = {
  [FF_TYPE_STANDARD]: 'standard',
  [FF_TYPE_OPTION]: 'option',
  [FF_TYPE_BOOLEAN]: 'boolean',
  [FF_TYPE_EVENT]: 'boolean',
  [FF_TYPE_INTEGER]: 'text',
  [FF_TYPE_TEXT]: 'text',
};

/**
 * The kit reads `embed`, `size`, `clip` and `bg` from the query string itself,
 * and this page reads `plugin`; a parameter id equal to one would be set by it.
 */
const RESERVED_QUERY = new Set(['embed', 'size', 'clip', 'bg', 'plugin']);
const addressOf = (name) => {
  const address = name.toLowerCase().replace(/\s+/g, '');
  return RESERVED_QUERY.has(address) ? `param-${address}` : address;
};

/** Left off the panel, and said so in the disclosure. */
const ABSENT = new Set(['Audio', 'Audio Drops', 'Audio Shakes']);
const skip = (d) => d.group === 'About' || d.type === FF_TYPE_BUFFER || ABSENT.has(d.name);

function kitDefault(d) {
  switch (d.type) {
    case FF_TYPE_OPTION: {
      const i = d.values.findIndex((v) => v === d.default);
      return i >= 0 ? i : Math.round(d.default);
    }
    case FF_TYPE_INTEGER: return String(Math.round(d.default));
    case FF_TYPE_BOOLEAN:
    case FF_TYPE_EVENT: return d.default > 0.5 ? 1 : 0;
    default: return d.default;
  }
}

/** @param ff {source?: declaration, over?: declaration} the same control in each plugin */
function kitParam(ff, id, only) {
  const d = ff.over ?? ff.source;
  const p = { id, name: d.name, type: KIT_TYPES[d.type], group: d.group, hint: HINTS[d.name], ff, only };
  if (p.type === undefined) throw new Error(`no kit control for FFGL type ${d.type} (${d.name})`);
  if (d.type === FF_TYPE_OPTION) p.elements = d.elements;
  if (d.type === FF_TYPE_STANDARD) p.display = readout(d);
  return p;
}

const sameDeclaration = (a, b) => JSON.stringify({ ...a, index: 0 }) === JSON.stringify({ ...b, index: 0 });

/**
 * One list for both plugins, in each plugin's own order: the Over's list is
 * walked, and a control the source declares and the Over does not (Lightbox)
 * is placed where the source puts it, before the next control they share.
 */
const PARAMS = [];
{
  const source = DECLARED.source.list;
  let flushed = 0;
  const flushSourceOnly = (upTo) => {
    for (; flushed < upTo; flushed += 1) {
      const s = source[flushed];
      if (!skip(s) && !DECLARED.over.list.some((d) => d.name === s.name)) PARAMS.push(kitParam({ source: s }, addressOf(s.name), 'source'));
    }
  };
  for (const d of DECLARED.over.list) {
    if (skip(d)) continue;
    const s = source.find((x) => x.name === d.name);
    if (s) flushSourceOnly(s.index);
    if (s && !skip(s) && sameDeclaration({ ...s, id: 0 }, { ...d, id: 0 })) {
      PARAMS.push(kitParam({ source: s, over: d }, addressOf(d.name)));
    } else {
      if (s && !skip(s)) PARAMS.push(kitParam({ source: s }, addressOf(s.name), 'source'));
      PARAMS.push(kitParam({ over: d }, addressOf(d.name) + (s ? '_over' : ''), 'over'));
    }
  }
  flushSourceOnly(source.length);
}

/** Each constructor's defaults, by kit id. */
const DEFAULTS = { source: {}, over: {} };
for (const p of PARAMS) {
  for (const variant of VARIANTS) DEFAULTS[variant][p.id] = kitDefault(p.ff[variant] ?? p.ff.over ?? p.ff.source);
}

const query = new URLSearchParams(window.location.search);
const initialVariant = query.get('plugin') === 'over' ? 'over' : 'source';
for (const p of PARAMS) p.default = DEFAULTS[initialVariant][p.id];

const paramByName = (name) => PARAMS.find((p) => p.name === name && !p.only) ?? PARAMS.find((p) => p.name === name);

//===========================================================================
// Presets: this page's settings, in the plugin's units, converted to the
// host's 0..1 by the plugin's own ParamFrom* functions (Controls.cpp) through
// glue.cpp. Options by element index, as the kit stores them.
//===========================================================================

const from = (name, value) => {
  const v = core._hd_param_from(idOf(name), value);
  if (Number.isNaN(v)) throw new Error(`the plugin has no inverse conversion for ${name}`);
  return v;
};
const option = (name, element) => {
  const d = DECLARED.over.list.find((x) => x.name === name) ?? DECLARED.source.list.find((x) => x.name === name);
  const i = d.elements.indexOf(element);
  if (i < 0) throw new Error(`${name} has no element ${element}`);
  return i;
};
/** Drop Position is two rows (the Over's list has Brightest); a preset sets both. */
const dropPosition = (element) => ({ dropposition: option('Drop Position', element), dropposition_over: option('Drop Position', element) });

const PRESETS = {
  'Ferroin BZ in a 100 mm Petri dish, Batch': {
    reaction: option('Reaction', 'Belousov-Zhabotinsky'),
    vessel: option('Vessel', 'Petri Dish'),
    dishwidth: from('Dish Width', 100),
    reactor: option('Reactor', 'Batch'),
  },
  'Ru(bpy)₃ BZ, photosensitive (the Over’s clip is light)': {
    reaction: option('Reaction', 'Belousov-Zhabotinsky'),
    catalyst: option('Catalyst', 'Ru(bpy)3'),
    lightcoupling: 1,
    seedfromclip: 1,
  },
  'Iodine clock, Batch, 10×, drops at the centre': {
    reaction: option('Reaction', 'Iodine Clock'),
    reactor: option('Reactor', 'Batch'),
    'time-lapse': from('Time-lapse', 10),
    ...dropPosition('Centre'),
  },
  'CDIMA Turing: 10 mm dish, Detail 512, 100×': {
    reaction: option('Reaction', 'CDIMA Turing'),
    dishwidth: from('Dish Width', 10),
    detail: option('Detail', '512'),
    'time-lapse': from('Time-lapse', 100),
  },
  'Blue bottle, 7 mm layer, Batch, 60× (press Shake)': {
    reaction: option('Reaction', 'Blue Bottle'),
    depth: from('Depth', 7),
    reactor: option('Reactor', 'Batch'),
    'time-lapse': from('Time-lapse', 60),
  },
  'Traffic light, 7 mm layer, Batch, 60× (press Shake)': {
    reaction: option('Reaction', 'Traffic Light'),
    depth: from('Depth', 7),
    reactor: option('Reactor', 'Batch'),
    'time-lapse': from('Time-lapse', 60),
  },
  'Vanishing valentine, 7 mm layer, Batch, 60× (press Shake)': {
    reaction: option('Reaction', 'Vanishing Valentine'),
    depth: from('Depth', 7),
    reactor: option('Reactor', 'Batch'),
    'time-lapse': from('Time-lapse', 60),
  },
  'Chemical chameleon, still, 10×, 6 mm drops at the centre (press Drop)': {
    reaction: option('Reaction', 'Chemical Chameleon'),
    reactor: option('Reactor', 'Batch'),
    'time-lapse': from('Time-lapse', 10),
    dropsize: from('Drop Size', 6),
    ...dropPosition('Centre'),
  },
  'Briggs–Rauscher, 7 mm layer, Batch, 60× (the CPU engine)': {
    reaction: option('Reaction', 'Briggs-Rauscher'),
    depth: from('Depth', 7),
    reactor: option('Reactor', 'Batch'),
    'time-lapse': from('Time-lapse', 60),
  },
};

//===========================================================================
// The recipe line: the plugin's CurrentRecipe() in mol/L, named by this page
// from the README's table.
//===========================================================================

const RECIPE_NAMES = [
  ['NaBrO₃', 'H⁺', 'malonic acid', 'catalyst'],
  ['KIO₃ (H₂O₂ follows it)', 'H⁺', 'malonic acid', 'starch sites'],
  ['H₂O₂', 'H⁺', 'Na₂S₂O₃', 'starch sites'],
  ['ClO₂', 'H⁺', 'malonic acid', 'starch/PVA sites'],
  ['O₂ (a Shake)', 'NaOH', 'glucose', 'indigo carmine'],
  ['O₂ (a Shake)', 'NaOH', 'glucose', 'methylene blue'],
  ['O₂ (a Shake)', 'NaOH', 'glucose', 'resazurin'],
  ['KMnO₄ per dose', 'NaOH', 'glucose', 'KMnO₄ at the start'],
];

function molar(m) {
  if (!(m > 0)) return 'none';
  if (m >= 0.1) return `${m.toFixed(2)} M`;
  if (m >= 1e-3) return `${(m * 1e3).toPrecision(2)} mM`;
  return `${(m * 1e6).toPrecision(2)} µM`;
}

//===========================================================================
// The renderer: one plugin instance, driven as a host drives it.
//===========================================================================

/** The value the host would send for a kit control, or null for none. */
function hostValue(d, v) {
  switch (d.type) {
    case FF_TYPE_OPTION: return Math.fround(d.values[Math.round(v)] ?? 0);
    case FF_TYPE_BOOLEAN: return v > 0.5 ? 1 : 0;
    case FF_TYPE_EVENT: return null;
    case FF_TYPE_INTEGER: {
      const n = Number.parseInt(String(v).trim(), 10);
      if (!Number.isFinite(n)) return null;
      return Math.min(d.max, Math.max(d.min, n));
    }
    default: return Math.fround(v);
  }
}

function createRenderer(gl) {
  // The state is RGBA32F sampled LINEAR by the stir pass and the Over's
  // thumbnail; the kit only requests this extension, and without it WebGL2
  // treats such a texture as incomplete and reads it as black.
  if (!gl.getExtension('OES_texture_float_linear')) {
    throw new GLError('OES_texture_float_linear is missing. The dish’s state is a 32-bit float texture the plugin samples with linear filtering, and without this extension WebGL2 would read it as black — a plausible wrong picture rather than an obvious one, so the demo stops here instead.');
  }

  // emscripten's GL library, pointed at the kit's context. Nothing is turned
  // on behind the kit's back: no extensions are enabled on its behalf.
  const handle = core.GL.registerContext(gl, { majorVersion: 2, minorVersion: 0, enableExtensionsByDefault: false });
  core.GL.makeContextCurrent(handle);
  core._hd_prepare_log();

  let instance = 0;
  let variant = null;
  let pushed = new Map();
  let releases = [];
  let clipName = 0;
  let clipTexture = null;
  let frames = 0;
  let bpm = 120;
  let lastSubsteps = 0;
  let substepsThisFrame = 0;

  /** A kit texture under a GL name the plugin can bind, in emscripten's tables. */
  function glName(texture) {
    if (texture === clipTexture) return clipName;
    if (clipName) core.GL.textures[clipName] = null;
    clipName = core.GL.getNewId(core.GL.textures);
    core.GL.textures[clipName] = texture;
    texture.name = clipName;
    clipTexture = texture;
    return clipName;
  }

  const indexOf = (p) => p.ff[variant]?.index ?? -1;

  function start(which, width, height) {
    if (instance) core._hd_delete(instance);
    instance = core._hd_new(which === 'over' ? 1 : 0);
    liveInstance = instance;
    variant = which;
    pushed = new Map();
    releases = [];
    frames = 0;
    lastSubsteps = 0;
    shaderCheck.refused = null;
    shaderCheck.compiled.clear();
    if (!core._hd_init_gl(instance, width, height)) {
      const why = shaderCheck.refused
        ? `the ${shaderCheck.refused.stage} shader text the compiled plugin handed to GL (${shaderCheck.refused.length} characters) is not one of this page’s checked copies of source/Shaders.cpp, so it was not compiled`
        : `the plugin’s InitGL failed${printed.length ? `: ${printed.slice(-3).join(' ')}` : ''} (its log is under the picture)`;
      throw new GLError(`${which === 'over' ? 'SW Honeydew Over' : 'SW Honeydew'} could not start: ${why}.`);
    }
    core._hd_set_beat_info(instance, bpm, NaN);
  }

  function push(params) {
    for (const p of PARAMS) {
      const d = p.ff[variant];
      if (!d || d.type === FF_TYPE_EVENT) continue;
      const value = hostValue(d, params.get(p.id));
      if (value === null || pushed.get(p.id) === value) continue;
      pushed.set(p.id, value);
      core._hd_set_float(instance, d.index, value);
    }
  }

  return {
    render({ input, params, width, height, time, variant: wanted }) {
      core.GL.makeContextCurrent(handle);
      if (!instance || wanted !== variant) start(wanted ?? 'source', width, height);
      push(params);
      const clip = variant === 'over' ? glName(input.texture) : 0;
      if (!core._hd_process(instance, time, clip, input.width, input.height)) {
        throw new GLError('The plugin’s ProcessOpenGL failed.');
      }
      // A press is held through exactly one frame: Resolume sends 1 on one
      // frame and 0 on the next, and the plugin reads the rising edge.
      for (const index of releases) core._hd_set_float(instance, index, 0);
      releases = [];
      const total = core._hd_substeps(instance);
      substepsThisFrame = total - lastSubsteps;
      lastSubsteps = total;
      // For anything checking the page from outside.
      frames += 1;
      gl.canvas.dataset.frames = String(frames);
      gl.canvas.dataset.chemTime = core._hd_chem_time(instance).toFixed(3);
      gl.canvas.dataset.substeps = String(substepsThisFrame);
      gl.canvas.dataset.plugin = variant;
    },
    /** Press an event control (by kit parameter): 1 now, 0 after the next frame. */
    press(p) {
      if (!instance) return false;
      const index = indexOf(p);
      if (index < 0) return false;
      core._hd_set_float(instance, index, 1);
      releases.push(index);
      return true;
    },
    setBpm(value) {
      bpm = value;
      if (instance) core._hd_set_beat_info(instance, bpm, NaN);
    },
    get variant() { return variant; },
    /** One texel of the state after the last frame (A: which 0, B: 1), as hdtest reads it. */
    texel(which, x, y, channel) {
      return instance ? core._hd_state_texel(instance, which, x, y, channel) : NaN;
    },
    /** The Briggs–Rauscher engine's mean of one of its ten species (BRIndex order), as --briggs reads it. */
    brMean(species) {
      return instance ? core._hd_br_mean(instance, species) : NaN;
    },
    /** The plugin's own accessors, the ones hdtest reads. */
    status() {
      if (!instance) return null;
      return {
        chemTime: core._hd_chem_time(instance),
        chemTimeTotal: core._hd_chem_time_total(instance),
        lostChem: core._hd_lost_chem(instance),
        cappedFrames: core._hd_capped_frames(instance),
        substeps: substepsThisFrame,
        hostDt: core._hd_host_dt(instance),
        cols: core._hd_grid_cols(instance),
        rows: core._hd_grid_rows(instance),
        cellMm: core._hd_cell_mm(instance),
        drops: core._hd_drops(instance),
        doses: core._hd_doses(instance),
        // The dish's mean state (MeanState), what Clock Sync decides on.
        mean: {
          a: [0, 1, 2, 3].map((i) => core._hd_mean(instance, 0, i)),
          b: [0, 1, 2, 3].map((i) => core._hd_mean(instance, 1, i)),
        },
        reaction: core._hd_reaction(instance),
        recipe: [0, 1, 2, 3].map((i) => core._hd_recipe(instance, i)),
        brSteps: core._hd_br_steps(instance),
        compiled: [...shaderCheck.compiled].sort().join(', '),
      };
    },
    log() {
      try {
        return core.FS.readFile(text(core._hd_log_path()), { encoding: 'utf8' });
      } catch {
        return '';
      }
    },
  };
}

//===========================================================================
// The page.
//===========================================================================

let renderer = null;

const mounted = mountDemo({
  name: 'Honeydew',
  pluginId: 'HD01 · HD02',
  kind: ['source', 'effect'],
  tagline:
    'Oscillating and clock reactions in a Petri dish on a lightbox, for Resolume: the Belousov–Zhabotinsky reaction’s target waves and spirals, the Briggs–Rauscher and iodine-clock colour changes, CDIMA Turing spots and stripes, the blue bottle’s fade and the chemical chameleon’s purple → green → brown, each from its published mechanism with its rate constants in mol/L and seconds, coloured by Beer–Lambert through each species’ spectrum against the CIE 1931 observer. SW Honeydew is the dish on its lightbox; SW Honeydew Over makes your clip the lightbox and, where the chemistry is photosensitive, lets the clip act on it.',
  repo: 'https://github.com/stoatworks-labs/honeydew',

  blurb:
    'It is the plugin’s own C++ — the plugin class, its chemistry, spectra, controls, clock and Briggs–Rauscher engine, with the FFGL SDK classes it uses — compiled unmodified to WebAssembly, running its own GLSL in WebGL2. The host is this page: no Resolume transport or audio, one thread, and SW Honeydew Over runs on a generated clip.',

  // The Over keeps the clip's alpha; the source is opaque.
  showBackdrop: true,

  variants: {
    label: 'Plugin',
    default: initialVariant,
    options: [
      { id: 'source', name: 'SW Honeydew (source)', hint: 'HD01, FF_SOURCE: the dish on its lightbox.' },
      { id: 'over', name: 'SW Honeydew Over (effect)', hint: 'HD02, FF_EFFECT: the clip is the lightbox, filtered by the layer; its light acts on Ru(bpy)₃ BZ and CDIMA.' },
    ],
  },

  // For SW Honeydew Over only.
  sources: ['scene', 'spot', 'bars', 'grid', 'alpha', 'detail'],

  params: PARAMS,
  presets: PRESETS,
  needFloat: true,

  differences: [
    'This page runs the plugin rather than a port of it. honeydew-core.wasm is source/Honeydew.cpp — the plugin class: its constructor, clock and unit vote, transport, buttons, grid, seed, substep plan, every pass and every uniform, Clock Sync, the drops — and everything it calls (Chemistry, Spectra, Controls, BrEngine, Audio, PassBuffer, Shaders, Diag), with the FFGL SDK’s CFFGLPlugin, parameter bookkeeping, FFGLShader and FFGLScreenQuad, all compiled unmodified by emscripten (demo/tools/build-wasm.sh). Only the two registration files are left out; the page constructs the plugin itself, as the plugin’s harness does. The panel is read back from the plugin’s own declarations through the SDK’s host getters, and the numbers beside the sliders are Controls.cpp’s conversions, compiled in; their units, the recipe line’s species names and every tooltip are this page’s.',
    'Both engines run as the plugin runs them. The GPU reactions — Belousov–Zhabotinsky as the three-variable Oregonator with ROS2 per texel, CDIMA Turing as Lengyel–Epstein, the iodine clock, the blue-bottle family and the chemical chameleon — run in the shipped GLSL under WebGL2 on RGBA32F state textures (the page needs EXT_color_buffer_float and OES_texture_float_linear and stops with a message without them). Briggs–Rauscher is the plugin’s CPU engine, the De Kepper–Epstein mechanism in double, compiled to WebAssembly; the plugin runs its cells on up to four worker threads where the machine has cores, and this build has no threads, so its own single-worker path runs them on the page’s thread. The result is the same for any thread count (the plugin says so of itself); a Briggs–Rauscher frame costs more here.',
    'The GLSL is the plugin’s: demo/shaders.js is spliced from source/Shaders.cpp and demo/tools/check_shaders.py fails if a character differs, or if the WebAssembly was built from sources that have since changed (demo/wasm/inputs.sha256). At start-up the page also requires every shader the compiled plugin hands to glShaderSource to equal one of the programs its copy assembles — the same table InitGL uses — and refuses to compile anything else. The kit’s port() then changes the version line and adds the ES precision defaults. GL is emscripten’s WebGL2 library on this page’s context, not a GL 4.1 driver, and six entry points are the page’s (demo/wasm/gl_shim.cpp): glShaderSource, for that check and port; glEnable, glDisable and glIsEnabled, because GL_PROGRAM_POINT_SIZE, state the plugin saves and restores each frame as a plugin in Resolume must, does not exist in WebGL2; and glTexImage2D with glGetTexImage, because WebGL2 reads a texture back only through a framebuffer, so the plugin’s two read-backs — the dish’s mean state, which Clock Sync decides on, and the Over’s 32 × 18 thumbnail for Drop Position Brightest — go through one.',
    'Arithmetic. WebAssembly has no fused multiply-add and WebGL2 is GLSL ES 3.00 on whatever GPU you have, so a period or a snap lands a frame or two from where the plugin’s harness measures it on a Mac — inside the bounds that harness itself allows for another GPU (AGENTS.md). The models and every constant are the same code.',
    'No transport. Resolume hands the plugin a BPM and a bar phase (SetBeatInfo); a browser has neither, so the plugin keeps its own phase at the Host BPM field under the picture (120 at first), as it does in a host with no transport. Clock Sync therefore lands on the plugin’s own beats and bars, not on anything you can hear.',
    'Nothing audio. The plugin declares an Audio FFT buffer that Resolume fills, with Audio Drops and Audio Shakes over its onsets. A browser has no Resolume FFT, so all three are absent from this panel rather than present and dead: with no spectrum the plugin’s analyser never fires, so leaving them out changes nothing the page draws.',
    'The clock is the page’s: seconds from frame deltas, capped at a tenth of a second, handed to SetTime; the plugin’s own vote settles the unit (seconds) within four frames, as in Arena. Pause stops its clock and Step advances it 1/60 s. Restart is a backward jump, which the plugin takes as no time passing — and this page also presses the plugin’s Reset on it, so Restart is a fresh dish. A Reset, Drop, Break Wave or Shake press is held through one frame and released on the next, as Resolume sends an event. Seed is FF_TYPE_INTEGER in the plugin and a number field here, 0–9999.',
    'Time-lapse and Detail cost what they cost here. A frame runs at most 96 chemistry substeps (the plugin’s own cap); past it the chemistry falls behind real time and the status line counts the dropped seconds, as the plugin’s log does. The default Detail is the plugin’s, 256 cells across; 1024 at 300× is a workstation GPU’s job, not a laptop browser’s.',
    'The presets are this page’s suggestions, not the plugin’s: settings in the plugin’s units (a 10 mm dish, a 7 mm layer, 60×) converted to the host’s 0..1 by the plugin’s own ParamFrom functions. Anything a preset does not name goes back to the constructor’s default.',
    'SW Honeydew Over’s clip is the kit’s generated one, premultiplied, or your own image or video; its whole texture is used, where a host may pad it. Switching the Plugin dropdown is a new instance at that constructor’s defaults, as in Resolume; Drop Position is shown as two rows of the same name because the Over’s list has one more element, Brightest, and the kit’s dropdowns have a fixed list. The About block (a text and three buttons) is declared by the plugin and left off this panel.',
    'The plugin itself has never been loaded into Resolume (its README’s Status says so). Its harness, hdtest, drives the same classes headlessly and holds the chemistry to its literature — the Oregonator’s period, the trigger wave’s speed, the clock’s snap, the Briggs–Rauscher period, the Turing wavelength, the Beer–Lambert integral — and that harness, not this page, is the reason to believe it. Nothing here is measured.',
  ],

  createRenderer: (gl) => {
    renderer = createRenderer(gl);
    return renderer;
  },
});

//===========================================================================
// The controls the kit has no type for, the host's BPM, the status line and
// the plugin's log.
//
// By inline style, not the `hidden` attribute: kit.css gives these elements a
// `display` of their own, which beats the attribute's user-agent rule.
//===========================================================================

if (mounted) {
  const params = mounted.params;
  const embed = query.has('embed') && query.get('embed') !== '0';
  let shownVariant = mounted.state.variant;

  // For anything checking the page from outside (a headless browser reading
  // the plugin's own accessors): the kit's params and state, and the renderer.
  window.honeydewDemo = { params, state: mounted.state, redraw: mounted.redraw, get renderer() { return renderer; } };

  // Copy link carries the plugin.
  const toQuery = params.toQuery.bind(params);
  params.toQuery = () => {
    const q = toQuery();
    if (mounted.state.variant === 'over') q.set('plugin', 'over');
    return q;
  };

  const show = (node, on) => { if (node) node.style.display = on ? '' : 'none'; };
  // A row once found is claimed by its kit id (data-claimed), because the
  // kit gives a boolean row no element id and two rows can share a name
  // (Drop Position): the first lookup goes by id, then by an unclaimed name.
  const rowOf = (p) => document.querySelector(`.prow[data-claimed="${p.id}"]`)
    ?? document.getElementById(`p-${p.id}`)?.closest('.prow')
    ?? [...document.querySelectorAll('.prow')].find((row) => row.querySelector('.prow__name')?.textContent === p.name && !row.dataset.claimed);

  function showForVariant(which) {
    for (const p of PARAMS) if (p.only) show(rowOf(p), p.only === which);
    for (const group of document.querySelectorAll('.pgroup')) {
      show(group, [...group.querySelectorAll('.prow')].some((row) => row.style.display !== 'none'));
    }
    const effect = which === 'over';
    for (const field of document.querySelectorAll('.transport__field')) {
      if (field.querySelector('.transport__label')?.textContent === 'Clip') show(field, effect);
    }
    show(document.querySelector('.transport__file'), effect);
  }

  document.addEventListener('demo:state', () => {
    const which = mounted.state.variant;
    if (which !== shownVariant) {
      shownVariant = which;
      // A new instance: that constructor's defaults, for Defaults too.
      params.defaults = { ...DEFAULTS[which] };
      params.reset();
    }
    if (!embed) showForVariant(which);
  });

  if (!embed) {
    for (const p of PARAMS) {
      const row = rowOf(p);
      if (row) row.dataset.claimed = p.id;
    }
    showForVariant(shownVariant);

    const button = (label, title) => {
      const b = document.createElement('button');
      b.type = 'button';
      b.className = 'btn';
      b.textContent = label;
      if (title) b.title = title;
      return b;
    };

    for (const p of PARAMS) {
      const row = document.querySelector(`.prow[data-claimed="${p.id}"]`);
      if (!row) continue;
      const d = p.ff.over ?? p.ff.source;

      // FF_TYPE_EVENT: a press held through one frame, then released.
      if (d.type === FF_TYPE_EVENT) {
        const toggle = row.querySelector('.prow__toggle');
        const press = button(p.name, p.hint);
        press.className = 'prow__toggle';
        press.style.textAlign = 'center';
        press.dataset.press = p.id;
        press.addEventListener('click', () => {
          if (renderer?.press(p)) mounted.redraw();
        });
        toggle?.replaceWith(press);
        continue;
      }

      // FF_TYPE_INTEGER: a number field in the declared range.
      if (d.type === FF_TYPE_INTEGER) {
        const field = row.querySelector('.prow__text');
        if (!field) continue;
        field.type = 'number';
        field.min = String(d.min);
        field.max = String(d.max);
        field.step = '1';
        field.inputMode = 'numeric';
      }
    }

    // Restart is a fresh dish: the kit puts its clock back (a jump the plugin
    // ignores), and this presses the plugin's own Reset.
    const reset = PARAMS.find((p) => p.name === 'Reset');
    for (const b of document.querySelectorAll('.transport .btn')) {
      if (b.textContent === 'Restart' && reset) b.addEventListener('click', () => { renderer?.press(reset); });
    }

    //-----------------------------------------------------------------------
    // The host's BPM: what Resolume's transport would hand the plugin.
    //-----------------------------------------------------------------------
    const transport = document.querySelector('.transport');
    if (transport) {
      const label = document.createElement('label');
      label.className = 'transport__field';
      const span = document.createElement('span');
      span.className = 'transport__label';
      span.textContent = 'Host BPM';
      const bpm = document.createElement('input');
      bpm.type = 'number';
      bpm.className = 'prow__text';
      bpm.min = '20';
      bpm.max = '300';
      bpm.step = '1';
      bpm.value = '120';
      bpm.inputMode = 'numeric';
      bpm.style.width = '6em';
      bpm.style.flex = '0 0 auto';
      bpm.id = 'honeydew-bpm';
      label.title = 'The BPM Resolume’s transport would hand the plugin (SetBeatInfo). The page has no bar phase, so the plugin runs its own; Clock Sync lands on it.';
      bpm.addEventListener('change', () => {
        const v = Number(bpm.value);
        if (Number.isFinite(v) && v >= 20 && v <= 300) renderer?.setBpm(v);
      });
      label.append(span, bpm);
      transport.append(label);
    }

    //-----------------------------------------------------------------------
    // What the plugin reports of itself (its harness's accessors), and its
    // own log (Diag.cpp). Both report; neither measures.
    //-----------------------------------------------------------------------
    const stage = document.querySelector('.stage');
    if (stage) {
      const status = document.createElement('p');
      status.className = 'stage__status';
      status.id = 'honeydew-status';
      status.setAttribute('aria-live', 'off');
      const log = document.createElement('p');
      log.className = 'stage__status';
      log.id = 'honeydew-log';
      log.setAttribute('aria-live', 'off');
      log.style.fontFamily = 'ui-monospace, SFMono-Regular, Menlo, monospace';
      log.style.fontSize = '12px';
      log.style.whiteSpace = 'pre-wrap';
      stage.append(status, log);
      let lastLog = '';
      setInterval(() => {
        const s = renderer?.status();
        if (s) {
          const names = RECIPE_NAMES[s.reaction] ?? ['oxidant', 'acid or base', 'reductant', 'indicator'];
          const recipe = names.map((n, i) => `${n} ${molar(s.recipe[i])}`).join(', ');
          const lapse = s.hostDt > 0 ? ` (${(s.hostDt > 0 ? core._hd_convert(idOf('Time-lapse'), Math.fround(params.get('time-lapse'))) : 0).toFixed(1)}× real time)` : '';
          const cap = s.cappedFrames > 0 ? ` The substep cap bit on ${s.cappedFrames} frame${s.cappedFrames === 1 ? '' : 's'}: ${s.lostChem.toFixed(1)} chemical s dropped, never banked.` : '';
          const br = s.reaction === 1 ? ` Briggs–Rauscher on the CPU engine: ${s.brSteps.toLocaleString()} ROS2 steps so far.` : '';
          status.textContent =
            `The plugin reports: ${s.chemTime.toFixed(1)} chemical s since the dish was fresh${lapse}; `
            + `a ${s.cols} × ${s.rows} grid of ${s.cellMm.toFixed(3)} mm cells; ${s.substeps} substep${s.substeps === 1 ? '' : 's'} last frame; `
            + `${s.drops} drop${s.drops === 1 ? '' : 's'}, ${s.doses} sized dose${s.doses === 1 ? '' : 's'}.${cap}${br} `
            + `Recipe at the sliders: ${recipe}.`;
        }
        const t = renderer?.log() ?? '';
        if (t === lastLog) return;
        lastLog = t;
        const lines = t.trim().split('\n').filter(Boolean).map((l) => l.replace(/^\S+ (INFO |WARN |ERROR) honeydew: /, '$1 '));
        log.textContent = lines.length
          ? `The plugin’s own log (Diag.cpp, in this page’s memory):\n${lines.slice(-3).join('\n')}`
          : '';
      }, 250);
    }
  }
}

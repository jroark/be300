import { parseLaunchConfig, parseMacAddress, normalizeBridgeUrl } from "./launch-config.mjs?v=20261022b";
import { BUTTON_MASK_SIZE, BUTTON_REGIONS } from "./button-regions.mjs?v=20261022b";

const ASSET_VERSION = "20261022b";
const worker = new Worker(new URL(`./worker.js?v=${ASSET_VERSION}`, import.meta.url), {
  type: "module",
});

// Hosted linux4be.com boots default to NE2000, the Stowaway keyboard dock,
// and the web glue's host-time RTC path. Speed 0 means unthrottled/full speed;
// non-zero values are pacing units, not MHz.
const DEFAULT_SDRAM_MB = 16;
const DEFAULT_SPEED = 0;

const HOSTED_NAND_IMAGES = [
  { id: "300", name: "300.bin", label: "WinCE 3.0", url: "./nand/300.bin", sdramMb: 16 },
  { id: "beshell", name: "beshell.bin", label: "BEshell", url: "./nand/beshell.bin", sdramMb: 16 },
  { id: "expod", name: "expod.bin", label: "Expod", url: "./nand/expod.bin", sdramMb: 16 },
  { id: "net", name: "net.bin", label: "CE.Net", url: "./nand/net.bin", sdramMb: 16 },
  { id: "mw-2.4", name: "mw-2.4.bin", label: "Microwindows (Linux 2.4.18)", url: "./nand/mw-2.4.bin", sdramMb: 16 },
  { id: "mw-2.6", name: "mw-2.6.bin", label: "Microwindows (Linux 2.6.8.1)", url: "./nand/mw-2.6.bin", sdramMb: 16 },
  { id: "mw-4.2", name: "mw-4.2.bin", label: "Microwindows (Linux 4.2.9)", url: "./nand/mw-4.2.bin", sdramMb: 16 },
  { id: "opie-2.4", name: "opie-2.4.bin", label: "OPIE (Linux 2.4.18)", url: "./nand/opie-2.4.bin", sdramMb: 16 },
  { id: "opie-2.6", name: "opie-2.6.bin", label: "OPIE (Linux 2.6.8.1)", url: "./nand/opie-2.6.bin", sdramMb: 16 },
  { id: "opie-4.2", name: "opie-4.2.bin", label: "OPIE (Linux 4.2.9)", url: "./nand/opie-4.2.bin", sdramMb: 16 },
  { id: "picogui-2.4", name: "picogui-2.4.bin", label: "PicoGUI (Linux 2.4.18)", url: "./nand/picogui-2.4.bin", sdramMb: 16 },
  { id: "picogui-2.6", name: "picogui-2.6.bin", label: "PicoGUI (Linux 2.6.8.1)", url: "./nand/picogui-2.6.bin", sdramMb: 16 },
  { id: "picogui-4.2", name: "picogui-4.2.bin", label: "PicoGUI (Linux 4.2.9)", url: "./nand/picogui-4.2.bin", sdramMb: 16 },
  { id: "glaze", name: "glaze.bin", label: "Glaze", url: "./nand/glaze.bin", sdramMb: 16 },
];

const nandSelect = document.querySelector("#nandSelect");
const nandFileInput = document.querySelector("#nandFile");
const nandFileRowEl = document.querySelector("#nandFileRow");
const nandFileNameEl = document.querySelector("#nandFileName");
const nandFileChangeBtn = document.querySelector("#nandFileChangeBtn");
const cbeaFileInput = document.querySelector("#cbeaFile");
const cbeaAddBtn = document.querySelector("#cbeaAddBtn");
const cbeaListEl = document.querySelector("#cbeaList");
const speedControlInput = document.querySelector("#speedControl");
const speedValueEl = document.querySelector("#speedValue");
const primarySocketSelect = document.querySelector("#primarySocket");
const cf0FileInput = document.querySelector("#cf0File");
const cf0PanelEl = document.querySelector("#cf0Panel");
const cf0FileNameEl = document.querySelector("#cf0FileName");
const cf0ChooseBtn = document.querySelector("#cf0ChooseBtn");
const netPanelEl = document.querySelector("#netPanel");
const cf1FileInput = document.querySelector("#cf1File");
const cfBlankPanel = document.querySelector("#cfBlankPanel");
const cfBlankSizeSelect = document.querySelector("#cfBlankSize");
const cfBlankFilesInput = document.querySelector("#cfBlankFiles");
const cfBlankFolderInput = document.querySelector("#cfBlankFolder");
const cfBlankAddFilesBtn = document.querySelector("#cfBlankAddFilesBtn");
const cfBlankAddFolderBtn = document.querySelector("#cfBlankAddFolderBtn");
const cfBlankClearBtn = document.querySelector("#cfBlankClearBtn");
const cfBlankListEl = document.querySelector("#cfBlankList");
const cfBlankSummaryEl = document.querySelector("#cfBlankSummary");
const netMacInput = document.querySelector("#netMac");
const netBridgeUrlInput = document.querySelector("#netBridgeUrl");
const targusKeyboardEnabledInput = document.querySelector("#targusKeyboardEnabled");
const audioEnabledInput = document.querySelector("#audioEnabled");
const bootBtn = document.querySelector("#bootBtn");
const stopBtn = document.querySelector("#stopBtn");
const resetBtn = document.querySelector("#resetBtn");
const downloadCfBtn = document.querySelector("#downloadCfBtn");
const clearSerialBtn = document.querySelector("#clearSerialBtn");
const nextBootNoteEl = document.querySelector("#nextBootNote");
const statusEl = document.querySelector("#status");
const serialEl = document.querySelector("#serial");
const consoleEl = document.querySelector("#console");
const deviceFrameEl = document.querySelector(".device-frame");
const screenEl = document.querySelector("#screen");
const screenCtx = screenEl.getContext("2d", { alpha: false });
const controlButtons = Array.from(document.querySelectorAll("[data-set][data-mask]"));

let uploadedNandFile = null;
// .cbea app packages queued for the next boot (src/cbea_install.h).
let pendingPackages = [];
// Files for a blank CF card, as { path, file } with backslash card paths.
let cfBlankFiles = [];
// Whether the worker's current machine has a slot-0 CF card to download.
let cfAttached = false;
let running = false;
let bootInFlight = false;
// Last fatal/boot error or invalid settings; drives the status dot.
let statusError = false;
// Restart reuses the worker's cached machine, so it needs one boot first.
let hasBooted = false;
// Hosted image to fall back to when "Your own image file…" is cancelled.
let lastHostedNand = nandSelect.value;
let imageData = screenCtx.createImageData(screenEl.width, screenEl.height);
let btnSet1 = 0;
let btnSet2 = 0;
let activeTargusKeyboardEnabled = false;
const pressedStowawayKeys = new Set();
let frameButtonMask = null;
let activeFrameButton = null;
let activeTouchPointerId = null;
let activeTouchPoint = { x: 0, y: 0 };
const activeMomentaryButtons = new Map();
const pendingMomentaryButtonReleases = new Map();
// Host keys held as device buttons while the Stowaway keyboard is off.
const pressedDeviceKeys = new Map();

const FRAME_BUTTON_MASK_URL = `./buttons_dpad_bw_mask.png?v=${ASSET_VERSION}`;
const BUTTON_MASK_THRESHOLD = 128;
const BUTTON_MIN_DWELL_MS = 120;

// Device buttons on the host keyboard when the Stowaway keyboard is off.
const DEVICE_KEY_MASKS = new Map([
  ["ArrowUp", 16], ["ArrowDown", 32], ["ArrowRight", 64], ["ArrowLeft", 128],
  ["Enter", 4], ["NumpadEnter", 4], ["Escape", 8],
]);

// Stowaway dock scancodes by KeyboardEvent.code; 256 + n sends n with the
// dock's Fn key held. Canonical table: src/stowaway_keys.def (kept in step by
// tests/test_stowaway_keymap.c).
const STOWAWAY_KEY_CODES = new Map([
  ["Digit1", 0], ["Digit2", 1], ["Digit3", 2], ["KeyZ", 3], ["Digit4", 4],
  ["Digit5", 5], ["Digit6", 6], ["Digit7", 7], ["KeyQ", 9], ["KeyW", 10],
  ["KeyE", 11], ["KeyR", 12], ["KeyT", 13], ["KeyY", 14], ["Backquote", 15],
  ["KeyX", 16], ["KeyA", 17], ["KeyS", 18], ["KeyD", 19], ["KeyF", 20],
  ["KeyG", 21], ["KeyH", 22], ["Space", 23], ["CapsLock", 24], ["Tab", 25],
  ["ControlLeft", 26], ["ControlRight", 26], ["AltLeft", 35],
  ["AltRight", 35], ["KeyC", 44], ["KeyV", 45], ["KeyB", 46], ["KeyN", 47],
  ["Minus", 48], ["Equal", 49], ["Backspace", 50], ["Digit8", 52],
  ["Digit9", 53], ["Digit0", 54], ["BracketLeft", 56], ["BracketRight", 57],
  ["Backslash", 58], ["KeyU", 60], ["KeyI", 61], ["KeyO", 62], ["KeyP", 63],
  ["Quote", 64], ["Enter", 65], ["NumpadEnter", 65], ["KeyJ", 68],
  ["KeyK", 69], ["KeyL", 70], ["Semicolon", 71], ["Slash", 72],
  ["ArrowUp", 73], ["KeyM", 76], ["Comma", 77], ["Period", 78],
  ["Delete", 80], ["ArrowLeft", 81], ["ArrowDown", 82], ["ArrowRight", 83],
  ["ShiftLeft", 88], ["ShiftRight", 89], ["Home", 256 + 81],
  ["End", 256 + 83], ["PageUp", 256 + 73], ["PageDown", 256 + 82], ["F1", 51],
  ["F2", 59], ["F3", 66], ["F4", 74], ["F5", 256 + 51], ["F6", 256 + 59],
  ["F7", 256 + 66], ["F8", 256 + 74],
]);

function setStatus(text) {
  statusEl.textContent = text;
}

function getSelectedHostedNand() {
  return HOSTED_NAND_IMAGES.find((image) => image.id === nandSelect.value) ?? HOSTED_NAND_IMAGES[0];
}

function getUploadedSdramMb(file) {
  return file?.name?.toLowerCase().includes("opie64") ? 64 : DEFAULT_SDRAM_MB;
}

function getActiveNand() {
  if (nandSelect.value === "upload") {
    return {
      kind: "upload",
      name: uploadedNandFile?.name ?? "",
      label: uploadedNandFile?.name ?? "your image",
      file: uploadedNandFile,
      sdramMb: getUploadedSdramMb(uploadedNandFile),
    };
  }
  const hosted = getSelectedHostedNand();
  return {
    kind: "fetch",
    name: hosted.name,
    label: hosted.label,
    url: hosted.url,
    sdramMb: hosted.sdramMb,
  };
}

function getActiveCf(slot) {
  if (slot === 0 && primarySocketSelect.value === "cfblank") {
    const sizeMb = getCfBlankSizeMb();
    const count = cfBlankFiles.length;
    return {
      kind: "blank",
      name: `blank ${sizeMb} MB card (${count} file${count === 1 ? "" : "s"})`,
      label: `a blank ${sizeMb} MB CF card`,
      sizeMb,
      files: cfBlankFiles.slice(),
    };
  }
  if (slot === 0 && primarySocketSelect.value !== "cf0") {
    return null;
  }
  const input = slot === 0 ? cf0FileInput : cf1FileInput;
  const file = input?.files?.[0] ?? null;
  return file ? { name: file.name, label: `CF card ${file.name}`, file } : null;
}

function getSpeedValue() {
  const value = Number.parseInt(speedControlInput.value, 10);
  return Number.isFinite(value) ? Math.min(64, Math.max(0, value)) : DEFAULT_SPEED;
}

function formatSpeed(value = getSpeedValue()) {
  return value === 0 ? "Full speed" : `${value} step batches`;
}

function syncSpeedLabel() {
  speedValueEl.textContent = formatSpeed();
}

function sendSpeed() {
  worker.postMessage({
    type: "setSpeed",
    targetMhz: getSpeedValue(),
  });
}

function syncPrimarySocketControls() {
  const primarySocket = primarySocketSelect.value;
  netPanelEl.hidden = primarySocket !== "ne2000";
  cf0PanelEl.hidden = primarySocket !== "cf0";
  cfBlankPanel.hidden = primarySocket !== "cfblank";
  const cf0File = cf0FileInput.files?.[0];
  cf0FileNameEl.textContent = cf0File
    ? `${cf0File.name} (${formatBytes(cf0File.size)})`
    : "No image chosen.";
}

function syncNandControls() {
  const usingUpload = nandSelect.value === "upload";
  nandFileRowEl.hidden = !usingUpload || !uploadedNandFile;
  nandFileNameEl.textContent = uploadedNandFile
    ? `${uploadedNandFile.name} (${formatBytes(uploadedNandFile.size)})`
    : "";
}

function getCfBlankSizeMb() {
  return Number.parseInt(cfBlankSizeSelect.value, 10) || 64;
}

// Cluster size cbea_fat_format() picks for each card size (src/cbea_install.c).
function getCfClusterBytes(sizeMb) {
  if (sizeMb <= 128) {
    return 2048;
  }
  return sizeMb <= 256 ? 4096 : 8192;
}

// Rough space the files take on the card: whole clusters per file plus a
// cluster per directory, against the data area left after FATs/root dir.
function estimateCfBlankUsage(sizeMb = getCfBlankSizeMb()) {
  const cluster = getCfClusterBytes(sizeMb);
  const dirs = new Set();
  let used = 0;
  for (const { path, file } of cfBlankFiles) {
    used += Math.ceil(file.size / cluster) * cluster;
    const parts = path.split("\\");
    for (let i = 1; i < parts.length; i++) {
      dirs.add(parts.slice(0, i).join("\\").toLowerCase());
    }
  }
  used += dirs.size * cluster;
  const capacity = sizeMb * 1024 * 1024 - 300 * 1024;
  return { used, capacity };
}

function formatBytes(bytes) {
  if (bytes >= 1024 * 1024) {
    return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
  }
  return bytes >= 1024 ? `${Math.ceil(bytes / 1024)} KB` : `${bytes} B`;
}

// FAT long names here are 8-bit (src/cbea_install.c dir_create), so drop
// characters FAT forbids or the card can't store.
function sanitizeCfComponent(name) {
  let out = "";
  for (const ch of name) {
    const code = ch.codePointAt(0);
    out += code < 0x20 || code > 0xFF || '\\/:*?"<>|'.includes(ch) ? "_" : ch;
  }
  out = out.replace(/[. ]+$/, "");
  return out || "_";
}

function toCfPath(relativePath) {
  return relativePath
    .split("/")
    .filter((part) => part && part !== "." && part !== "..")
    .map(sanitizeCfComponent)
    .join("\\");
}

function addCfBlankFiles(entries) {
  for (const { path, file } of entries) {
    const cfPath = toCfPath(path);
    if (!cfPath) {
      continue;
    }
    const key = cfPath.toLowerCase();
    cfBlankFiles = cfBlankFiles.filter((queued) => queued.path.toLowerCase() !== key);
    cfBlankFiles.push({ path: cfPath, file });
  }
  cfBlankFiles.sort((a, b) => a.path.localeCompare(b.path));
  renderCfBlankFiles();
  updateReadyStatus();
  syncButtons();
}

function renderCfBlankFiles() {
  cfBlankListEl.replaceChildren(...cfBlankFiles.map((entry) => {
    const item = document.createElement("li");
    const label = document.createElement("span");
    label.className = "file-name";
    label.textContent = `\\${entry.path} (${formatBytes(entry.file.size)})`;
    const remove = document.createElement("button");
    remove.type = "button";
    remove.textContent = "Remove";
    remove.addEventListener("click", () => {
      cfBlankFiles = cfBlankFiles.filter((queued) => queued !== entry);
      renderCfBlankFiles();
      updateReadyStatus();
      syncButtons();
    });
    item.append(label, remove);
    return item;
  }));
  const { used, capacity } = estimateCfBlankUsage();
  cfBlankSummaryEl.textContent = cfBlankFiles.length
    ? `${cfBlankFiles.length} file(s), about ${formatBytes(used)} of ${formatBytes(capacity)}. ` +
      "Files appear under \\Storage Card. Drop files or folders here."
    : "Empty card. Add files or folders, or drop them here. They appear under \\Storage Card.";
  cfBlankClearBtn.disabled = cfBlankFiles.length === 0;
}

// Walk dropped folders through the FileSystemEntry API.
async function collectDroppedEntries(entry, prefix, out) {
  if (entry.isFile) {
    const file = await new Promise((resolve, reject) => entry.file(resolve, reject));
    out.push({ path: `${prefix}${entry.name}`, file });
    return;
  }
  if (!entry.isDirectory) {
    return;
  }
  const reader = entry.createReader();
  for (;;) {
    const batch = await new Promise((resolve, reject) => reader.readEntries(resolve, reject));
    if (!batch.length) {
      break;
    }
    for (const child of batch) {
      await collectDroppedEntries(child, `${prefix}${entry.name}/`, out);
    }
  }
}

// The worker posts buzzer + wave output as 44100 Hz mono S16 chunks. Each
// chunk is scheduled back to back on an AudioContext created from a user
// gesture (Boot click or the Sound checkbox), as browser autoplay rules need.
const AUDIO_SAMPLE_RATE = 44100;
const AUDIO_LEAD_S = 0.05;
const AUDIO_MAX_AHEAD_S = 0.3;
let audioCtx = null;
let audioNextTime = 0;

function ensureAudioContext() {
  if (!audioEnabledInput.checked) {
    return;
  }
  const AudioContextCtor = window.AudioContext || window.webkitAudioContext;
  if (!AudioContextCtor) {
    return;
  }
  if (!audioCtx) {
    try {
      audioCtx = new AudioContextCtor({ sampleRate: AUDIO_SAMPLE_RATE });
    } catch {
      // Buffers stay at 44100 Hz; the browser resamples them.
      audioCtx = new AudioContextCtor();
    }
  }
  if (audioCtx.state === "suspended") {
    void audioCtx.resume().catch(() => {});
  }
}

function playAudio(samples) {
  if (!audioCtx || audioCtx.state !== "running" || !audioEnabledInput.checked ||
      !samples || samples.length === 0) {
    return;
  }
  const now = audioCtx.currentTime;
  if (audioNextTime < now + 0.01) {
    audioNextTime = now + AUDIO_LEAD_S;
  }
  if (audioNextTime - now > AUDIO_MAX_AHEAD_S) {
    // The guest is producing faster than real time; drop to bound latency.
    return;
  }
  const buffer = audioCtx.createBuffer(1, samples.length, AUDIO_SAMPLE_RATE);
  const channel = buffer.getChannelData(0);
  for (let i = 0; i < samples.length; i++) {
    channel[i] = samples[i] / 32768;
  }
  const source = audioCtx.createBufferSource();
  source.buffer = buffer;
  source.connect(audioCtx.destination);
  source.start(audioNextTime);
  audioNextTime += buffer.duration;
}

function getBootOptions() {
  const mac = parseMacAddress(netMacInput.value);
  const bridge = normalizeBridgeUrl(netBridgeUrlInput.value);
  const primarySocket = primarySocketSelect.value;

  return {
    primarySocket,
    speed: getSpeedValue(),
    cf0: getActiveCf(0),
    cf1: getActiveCf(1),
    enableNe2000: primarySocket === "ne2000",
    enableTargusKeyboard: targusKeyboardEnabledInput.checked,
    enableAudio: audioEnabledInput.checked,
    mac,
    bridge,
  };
}

function validateBootOptions(options = getBootOptions()) {
  if (options.cf0?.kind === "blank") {
    const { used, capacity } = estimateCfBlankUsage(options.cf0.sizeMb);
    if (used > capacity) {
      return {
        valid: false,
        message: `The CF files need about ${formatBytes(used)}, more than a ${options.cf0.sizeMb} MB card holds. Choose a bigger card or remove files.`,
      };
    }
  }
  if (nandSelect.value === "upload" && !uploadedNandFile) {
    return {
      valid: false,
      message: "Choose a system image file, or pick one of the hosted images.",
    };
  }
  if (options.primarySocket === "cf0" && !options.cf0) {
    return {
      valid: false,
      message: "Choose a CF card image file, or pick a different card.",
    };
  }
  if (options.enableNe2000 && !options.mac.valid) {
    return {
      valid: false,
      message: "NE2000 MAC must be a unicast address like 10:20:30:00:00:10.",
    };
  }
  if (options.enableNe2000 && !options.bridge.valid) {
    return {
      valid: false,
      message: "Network bridge URL must start with ws:// or wss://.",
    };
  }
  return { valid: true, message: "" };
}

function describeAccessories(options = getBootOptions()) {
  const parts = [];
  if (options.cf0) {
    parts.push(options.cf0.label);
  }
  if (options.cf1) {
    parts.push(`a second CF card`);
  }
  if (options.enableNe2000) {
    parts.push(options.bridge.url ? "a bridged NE2000 card" : "an NE2000 card");
  }
  if (options.enableTargusKeyboard) {
    parts.push("the keyboard");
  }
  return parts;
}

function joinList(parts) {
  if (parts.length <= 1) {
    return parts.join("");
  }
  return `${parts.slice(0, -1).join(", ")} and ${parts[parts.length - 1]}`;
}

function syncStatusState() {
  statusEl.dataset.state = bootInFlight ? "booting"
    : running ? "running"
    : statusError ? "error"
    : "idle";
}

function updateReadyStatus() {
  if (bootInFlight || running) {
    return;
  }
  const options = getBootOptions();
  const validation = validateBootOptions(options);
  statusError = !validation.valid;
  syncStatusState();
  if (!validation.valid) {
    setStatus(validation.message);
    return;
  }
  const active = getActiveNand();
  const accessories = describeAccessories(options);
  const suffix = accessories.length ? ` with ${joinList(accessories)}` : "";
  const memory = active.sdramMb !== DEFAULT_SDRAM_MB ? ` (${active.sdramMb} MB RAM)` : "";
  const speed = options.speed ? ` at ${formatSpeed(options.speed)}` : "";
  const apps = pendingPackages.length
    ? ` ${pendingPackages.length} app${pendingPackages.length === 1 ? "" : "s"} will be installed first.`
    : "";
  setStatus(`Ready to boot ${active.label}${memory}${suffix}${speed}.${apps}`);
}

function syncButtons() {
  const validation = validateBootOptions();
  // We always have at least the default fetch URL available, so Boot is
  // enabled until we're actually mid-boot. If the fetch fails the worker
  // will surface the error.
  bootBtn.disabled = bootInFlight || !validation.valid;
  stopBtn.disabled = !running;
  resetBtn.disabled = bootInFlight || !hasBooted;
  downloadCfBtn.disabled = bootInFlight;
  downloadCfBtn.hidden = !cfAttached || !(running || bootInFlight);
  nextBootNoteEl.hidden = !running;
  syncStatusState();
}

function clamp(value, min, max) {
  return Math.min(max, Math.max(min, value));
}

function sendButtons() {
  worker.postMessage({
    type: "setButtons",
    btnSet1,
    btnSet2,
  });
}

function isMaskActive(targetSet, mask) {
  return targetSet === "btn1" ? (btnSet1 & mask) !== 0 : (btnSet2 & mask) !== 0;
}

function syncControlButtonStates() {
  for (const button of controlButtons) {
    const targetSet = button.dataset.set;
    const mask = Number(button.dataset.mask);
    const active = isMaskActive(targetSet, mask);
    button.classList.toggle("control-active", active);
    button.setAttribute("aria-pressed", active ? "true" : "false");
  }
}

function setButtonMask(targetSet, mask, enabled) {
  if (targetSet === "btn1") {
    btnSet1 = enabled ? (btnSet1 | mask) : (btnSet1 & ~mask);
  } else {
    btnSet2 = enabled ? (btnSet2 | mask) : (btnSet2 & ~mask);
  }

  syncControlButtonStates();
  sendButtons();
}

function buttonPressKey(targetSet, mask) {
  return `${targetSet}:${mask}`;
}

function pressMomentaryButton(targetSet, mask) {
  const key = buttonPressKey(targetSet, mask);
  const pendingRelease = pendingMomentaryButtonReleases.get(key);
  if (pendingRelease) {
    clearTimeout(pendingRelease);
    pendingMomentaryButtonReleases.delete(key);
  }
  activeMomentaryButtons.set(key, performance.now());
  setButtonMask(targetSet, mask, true);
}

function releaseMomentaryButton(targetSet, mask) {
  const key = buttonPressKey(targetSet, mask);
  const pressedAt = activeMomentaryButtons.get(key);
  if (typeof pressedAt !== "number") {
    setButtonMask(targetSet, mask, false);
    return;
  }

  const finishRelease = () => {
    pendingMomentaryButtonReleases.delete(key);
    activeMomentaryButtons.delete(key);
    setButtonMask(targetSet, mask, false);
  };
  const remaining = BUTTON_MIN_DWELL_MS - (performance.now() - pressedAt);
  if (remaining <= 0) {
    finishRelease();
  } else {
    pendingMomentaryButtonReleases.set(key, setTimeout(finishRelease, remaining));
  }
}

function cancelMomentaryButtonTimers() {
  for (const pendingRelease of pendingMomentaryButtonReleases.values()) {
    clearTimeout(pendingRelease);
  }
  pendingMomentaryButtonReleases.clear();
  activeMomentaryButtons.clear();
}

function clearGuestButtons() {
  cancelMomentaryButtonTimers();
  pressedDeviceKeys.clear();
  btnSet1 = 0;
  btnSet2 = 0;
  syncControlButtonStates();
  sendButtons();
}

function sendStowawayKey(scancode, release) {
  worker.postMessage({
    type: "stowawayKey",
    scancode,
    release,
  });
}

function releasePressedStowawayKeys() {
  if (!pressedStowawayKeys.size) {
    return;
  }
  for (const code of pressedStowawayKeys) {
    const scancode = STOWAWAY_KEY_CODES.get(code);
    if (typeof scancode === "number") {
      sendStowawayKey(scancode, true);
    }
  }
  pressedStowawayKeys.clear();
}

function requestStop() {
  cancelActiveTouch();
  releasePressedStowawayKeys();
  clearGuestButtons();
  worker.postMessage({ type: "stop" });
  setStatus("Stopping emulator...");
}

function isEditableTarget(target) {
  if (!(target instanceof Element)) {
    return false;
  }
  const tagName = target.tagName.toLowerCase();
  return target.isContentEditable ||
    tagName === "input" ||
    tagName === "select" ||
    tagName === "textarea" ||
    tagName === "button" ||
    tagName === "summary";
}

function mapPointerToGuest(event) {
  const rect = screenEl.getBoundingClientRect();
  const x = clamp(Math.round(((event.clientX - rect.left) / rect.width) * 239), 0, 239);
  const y = clamp(Math.round(((event.clientY - rect.top) / rect.height) * 319), 0, 319);
  return { x, y };
}

function mapPointerToFrame(event) {
  const rect = deviceFrameEl.getBoundingClientRect();
  if (rect.width <= 0 || rect.height <= 0) {
    return null;
  }
  const x = ((event.clientX - rect.left) / rect.width) * 1024;
  const y = ((event.clientY - rect.top) / rect.height) * 1536;
  if (x < 0 || y < 0 || x >= 1024 || y >= 1536) {
    return null;
  }
  return { x, y };
}

function regionContains(region, x, y) {
  return x >= region.minX && x <= region.maxX &&
    y >= region.minY && y <= region.maxY;
}

function regionDistanceSq(region, x, y) {
  return ((x + 0.5 - region.cx) ** 2) + ((y + 0.5 - region.cy) ** 2);
}

function chooseSideButton(upper, lower, x, y, upperHit, lowerHit) {
  return regionDistanceSq(upper, x, y) <= regionDistanceSq(lower, x, y)
    ? upperHit
    : lowerHit;
}

function hitDpadRegion(region, x, y) {
  const rx = (region.maxX - region.minX + 1) / 2;
  const ry = (region.maxY - region.minY + 1) / 2;
  const dx = (x + 0.5 - region.cx) / rx;
  const dy = (y + 0.5 - region.cy) / ry;

  if ((dx * dx) + (dy * dy) < 0.18) {
    return { targetSet: "btn1", mask: 4 };
  }
  if (Math.abs(dy) >= Math.abs(dx)) {
    return { targetSet: "btn1", mask: dy < 0 ? 16 : 32 };
  }
  return { targetSet: "btn1", mask: dx > 0 ? 64 : 128 };
}

function hitFrameButton(point) {
  if (!frameButtonMask) {
    return null;
  }
  const x = Math.floor(point.x);
  const y = Math.floor(point.y);
  if (x < 0 || y < 0 || x >= frameButtonMask.width || y >= frameButtonMask.height) {
    return null;
  }
  if (!frameButtonMask.active[y * frameButtonMask.width + x]) {
    return null;
  }

  const regions = frameButtonMask.regions;
  if (regionContains(regions.dpad, x, y)) {
    return hitDpadRegion(regions.dpad, x, y);
  }
  if (regionContains(regions.rocket, x, y) && regionContains(regions.ok, x, y)) {
    return chooseSideButton(
      regions.rocket,
      regions.ok,
      x,
      y,
      { targetSet: "btn2", mask: 16 },
      { targetSet: "btn1", mask: 4 },
    );
  }
  if (regionContains(regions.power, x, y) && regionContains(regions.esc, x, y)) {
    return chooseSideButton(
      regions.power,
      regions.esc,
      x,
      y,
      { targetSet: "btn2", mask: 128 },
      { targetSet: "btn1", mask: 8 },
    );
  }
  if (regionContains(regions.rocket, x, y)) {
    return { targetSet: "btn2", mask: 16 };
  }
  if (regionContains(regions.power, x, y)) {
    return { targetSet: "btn2", mask: 128 };
  }
  if (regionContains(regions.ok, x, y)) {
    return { targetSet: "btn1", mask: 4 };
  }
  if (regionContains(regions.esc, x, y)) {
    return { targetSet: "btn1", mask: 8 };
  }
  return null;
}

function loadFrameButtonMask() {
  const image = new Image();
  image.addEventListener("load", () => {
    const width = image.naturalWidth;
    const height = image.naturalHeight;
    const canvas = document.createElement("canvas");
    canvas.width = width;
    canvas.height = height;
    const ctx = canvas.getContext("2d", { willReadFrequently: true });

    try {
      ctx.drawImage(image, 0, 0);
      const data = ctx.getImageData(0, 0, width, height).data;
      const active = new Uint8Array(width * height);
      for (let i = 0; i < active.length; i++) {
        const off = i * 4;
        const luma = (data[off] * 77) + (data[off + 1] * 150) + (data[off + 2] * 29);
        active[i] = data[off + 3] >= 16 && (luma >> 8) >= BUTTON_MASK_THRESHOLD ? 1 : 0;
      }
      if (width !== BUTTON_MASK_SIZE.width || height !== BUTTON_MASK_SIZE.height) {
        console.warn("BE-300 button mask size does not match button-regions.mjs.");
        return;
      }
      // Regions come from the desktop's analysis of this mask
      // (src/ui_button_mask.c via tests/test_button_mask.c).
      frameButtonMask = { active, width, height, regions: BUTTON_REGIONS };
    } catch (error) {
      console.warn("Unable to load BE-300 button mask.", error);
    }
  });
  image.addEventListener("error", () => {
    console.warn(`Unable to load BE-300 button mask: ${FRAME_BUTTON_MASK_URL}`);
  });
  image.src = FRAME_BUTTON_MASK_URL;
}

function appendSerial(text) {
  serialEl.textContent += text;
  if (serialEl.textContent.length > 120000) {
    serialEl.textContent = serialEl.textContent.slice(-80000);
  }
  serialEl.scrollTop = serialEl.scrollHeight;
  if (!consoleEl.open) {
    consoleEl.classList.add("has-new");
  }
}

consoleEl.addEventListener("toggle", () => {
  if (consoleEl.open) {
    consoleEl.classList.remove("has-new");
    serialEl.scrollTop = serialEl.scrollHeight;
  }
});

nandSelect.addEventListener("change", () => {
  if (nandSelect.value === "upload") {
    nandFileInput.click();
  } else {
    lastHostedNand = nandSelect.value;
  }
  syncNandControls();
  updateReadyStatus();
  syncButtons();
});

nandFileInput.addEventListener("change", () => {
  const file = nandFileInput.files?.[0] ?? null;
  if (file) {
    uploadedNandFile = file;
  }
  syncNandControls();
  updateReadyStatus();
  syncButtons();
});

// Dismissing the picker without a file goes back to the last hosted image.
nandFileInput.addEventListener("cancel", () => {
  if (!uploadedNandFile) {
    nandSelect.value = lastHostedNand;
  }
  syncNandControls();
  updateReadyStatus();
  syncButtons();
});

nandFileChangeBtn.addEventListener("click", () => nandFileInput.click());

function renderPendingPackages() {
  cbeaListEl.replaceChildren(...pendingPackages.map((file) => {
    const item = document.createElement("li");
    const label = document.createElement("span");
    label.className = "file-name";
    label.textContent = file.name;
    const remove = document.createElement("button");
    remove.type = "button";
    remove.textContent = "Remove";
    remove.addEventListener("click", () => {
      pendingPackages = pendingPackages.filter((queued) => queued !== file);
      renderPendingPackages();
      updateReadyStatus();
    });
    item.append(label, remove);
    return item;
  }));
}

async function readPackages(files) {
  return Promise.all(files.map(async (file) => ({
    name: file.name,
    bytes: await file.arrayBuffer(),
  })));
}

// Before boot, packages are queued and installed into the NAND image when
// Boot is pressed. While running, the worker installs them and reboots.
async function addPackages(fileList) {
  const files = Array.from(fileList).filter((file) => file.name.toLowerCase().endsWith(".cbea"));
  if (!files.length) {
    setStatus("Only Casio .cbea app packages can be installed (e.g. MAIN/wmp1.1.cbea).");
    return;
  }
  if (running) {
    const names = files.map((file) => file.name).join(", ");
    if (!window.confirm(`Install ${names} and reboot the emulator?`)) {
      return;
    }
    const packages = await readPackages(files);
    worker.postMessage({ type: "installPackages", packages }, packages.map((pkg) => pkg.bytes));
    return;
  }
  for (const file of files) {
    pendingPackages = pendingPackages.filter((queued) => queued.name !== file.name);
    pendingPackages.push(file);
  }
  renderPendingPackages();
  updateReadyStatus();
}

cbeaAddBtn.addEventListener("click", () => cbeaFileInput.click());

cbeaFileInput.addEventListener("change", async () => {
  if (cbeaFileInput.files?.length) {
    await addPackages(cbeaFileInput.files);
  }
  cbeaFileInput.value = "";
});

function isFileDrag(event) {
  return Array.from(event.dataTransfer?.types ?? []).includes("Files");
}

function isFileInputTarget(event) {
  return event.target instanceof HTMLInputElement && event.target.type === "file";
}

document.addEventListener("dragover", (event) => {
  if (!isFileDrag(event) || isFileInputTarget(event)) {
    return;
  }
  event.preventDefault();
  event.dataTransfer.dropEffect = "copy";
  document.body.classList.add("drag-active");
});

document.addEventListener("dragleave", (event) => {
  if (event.relatedTarget === null) {
    document.body.classList.remove("drag-active");
  }
});

cfBlankPanel.addEventListener("dragover", (event) => {
  if (!isFileDrag(event)) {
    return;
  }
  event.preventDefault();
  event.stopPropagation();
  event.dataTransfer.dropEffect = "copy";
  cfBlankPanel.classList.add("drop-active");
});

cfBlankPanel.addEventListener("dragleave", (event) => {
  if (!cfBlankPanel.contains(event.relatedTarget)) {
    cfBlankPanel.classList.remove("drop-active");
  }
});

cfBlankPanel.addEventListener("drop", async (event) => {
  cfBlankPanel.classList.remove("drop-active");
  document.body.classList.remove("drag-active");
  if (!isFileDrag(event)) {
    return;
  }
  event.preventDefault();
  event.stopPropagation();
  // Grab the entries synchronously; DataTransfer is emptied after the event.
  const items = Array.from(event.dataTransfer.items ?? []);
  const entries = items.map((item) => item.webkitGetAsEntry?.()).filter(Boolean);
  const files = Array.from(event.dataTransfer.files);
  const collected = [];
  try {
    if (entries.length) {
      for (const entry of entries) {
        await collectDroppedEntries(entry, "", collected);
      }
    } else {
      collected.push(...files.map((file) => ({ path: file.name, file })));
    }
  } catch (error) {
    setStatus(`Could not read dropped files: ${error instanceof Error ? error.message : String(error)}`);
    return;
  }
  addCfBlankFiles(collected);
});

cfBlankAddFilesBtn.addEventListener("click", () => cfBlankFilesInput.click());
cfBlankAddFolderBtn.addEventListener("click", () => cfBlankFolderInput.click());

cfBlankFilesInput.addEventListener("change", () => {
  addCfBlankFiles(Array.from(cfBlankFilesInput.files ?? [], (file) => ({ path: file.name, file })));
  cfBlankFilesInput.value = "";
});

cfBlankFolderInput.addEventListener("change", () => {
  addCfBlankFiles(Array.from(cfBlankFolderInput.files ?? [], (file) => ({
    path: file.webkitRelativePath || file.name,
    file,
  })));
  cfBlankFolderInput.value = "";
});

cfBlankClearBtn.addEventListener("click", () => {
  cfBlankFiles = [];
  renderCfBlankFiles();
  updateReadyStatus();
  syncButtons();
});

cfBlankSizeSelect.addEventListener("change", () => {
  renderCfBlankFiles();
  updateReadyStatus();
  syncButtons();
});

downloadCfBtn.addEventListener("click", () => {
  worker.postMessage({ type: "exportCf", slot: 0 });
});

function downloadCfImage(bytes) {
  const url = URL.createObjectURL(new Blob([bytes], { type: "application/octet-stream" }));
  const link = document.createElement("a");
  link.href = url;
  link.download = "be300-cf.img";
  document.body.append(link);
  link.click();
  link.remove();
  setTimeout(() => URL.revokeObjectURL(url), 0);
}

document.addEventListener("drop", (event) => {
  document.body.classList.remove("drag-active");
  if (!isFileDrag(event) || isFileInputTarget(event)) {
    return;
  }
  event.preventDefault();
  addPackages(event.dataTransfer.files);
});

for (const input of [
  speedControlInput,
  primarySocketSelect,
  cf1FileInput,
  netMacInput,
  netBridgeUrlInput,
  targusKeyboardEnabledInput,
]) {
  input.addEventListener("input", () => {
    syncSpeedLabel();
    if (input === speedControlInput) {
      sendSpeed();
    }
    syncPrimarySocketControls();
    updateReadyStatus();
    syncButtons();
  });
  input.addEventListener("change", () => {
    if (input === primarySocketSelect && primarySocketSelect.value === "cf0") {
      cf0FileInput.click();
    }
    syncSpeedLabel();
    if (input === speedControlInput) {
      sendSpeed();
    }
    syncPrimarySocketControls();
    updateReadyStatus();
    syncButtons();
  });
}

audioEnabledInput.addEventListener("change", () => {
  ensureAudioContext();
  worker.postMessage({ type: "setAudio", enabled: audioEnabledInput.checked });
});

cf0ChooseBtn.addEventListener("click", () => cf0FileInput.click());

cf0FileInput.addEventListener("change", () => {
  if (cf0FileInput.files?.[0]) {
    primarySocketSelect.value = "cf0";
  }
  syncPrimarySocketControls();
  updateReadyStatus();
  syncButtons();
});

async function bootMachine(userInitiated = true) {
  if (bootInFlight) return;
  const active = getActiveNand();
  const options = getBootOptions();
  const validation = validateBootOptions(options);
  if (!validation.valid) {
    setStatus(validation.message);
    syncButtons();
    return;
  }

  cancelActiveTouch();
  if (userInitiated) ensureAudioContext();
  statusError = false;
  hasBooted = true;
  bootInFlight = true;
  running = false;
  activeTargusKeyboardEnabled = options.enableTargusKeyboard;
  releasePressedStowawayKeys();
  clearGuestButtons();
  serialEl.textContent = "";
  setStatus(`Booting ${active.label}...`);
  syncButtons();

  const message = {
    type: "bootNand",
    nandName: active.name,
    sdramMb: active.sdramMb,
    targetMhz: options.speed,
    enableNe2000: options.enableNe2000,
    enableTargusKeyboard: options.enableTargusKeyboard,
    enableAudio: options.enableAudio,
    netMac: options.mac.bytes ? Array.from(options.mac.bytes) : null,
    netBridgeUrl: options.enableNe2000 ? options.bridge.url : "",
    cfSlot0Name: options.cf0?.name || "",
    cfSlot1Name: options.cf1?.name || "",
  };
  const transfer = [];

  try {
    if (pendingPackages.length) {
      message.packages = await readPackages(pendingPackages);
      transfer.push(...message.packages.map((pkg) => pkg.bytes));
    }
    if (active.kind === "upload") {
      message.nandBytes = await active.file.arrayBuffer();
      transfer.push(message.nandBytes);
    } else {
      message.nandUrl = `${active.url}?v=${ASSET_VERSION}`;
    }
    if (options.cf0?.kind === "blank") {
      const files = await Promise.all(options.cf0.files.map(async ({ path, file }) => ({
        path,
        bytes: await file.arrayBuffer(),
      })));
      message.cfSlot0Blank = { sizeMb: options.cf0.sizeMb, files };
      transfer.push(...files.map((file) => file.bytes));
    } else if (options.cf0) {
      message.cfSlot0Bytes = await options.cf0.file.arrayBuffer();
      transfer.push(message.cfSlot0Bytes);
    }
    if (options.cf1) {
      message.cfSlot1Bytes = await options.cf1.file.arrayBuffer();
      transfer.push(message.cfSlot1Bytes);
    }
  } catch (error) {
    bootInFlight = false;
    statusError = true;
    const messageText = `Boot failed: ${error instanceof Error ? error.message : String(error)}`;
    setStatus(messageText);
    appendSerial(`\n[FATAL] ${messageText}\n`);
    syncButtons();
    return;
  }

  cfAttached = Boolean(options.cf0);
  if (transfer.length) {
    worker.postMessage(message, transfer);
  } else {
    worker.postMessage(message);
  }
  // Installed packages persist in the worker's cached NAND for Reset.
  pendingPackages = [];
  renderPendingPackages();
}

bootBtn.addEventListener("click", () => { void bootMachine(); });

// Automatic boots run without a gesture. Start/resume sound on the first
// pointer or keyboard interaction rather than delaying the guest boot.
for (const type of ["pointerdown", "keydown"]) {
  window.addEventListener(type, () => {
    if (hasBooted) ensureAudioContext();
  }, { capture: true });
}

stopBtn.addEventListener("click", () => {
  requestStop();
});

resetBtn.addEventListener("click", () => {
  cancelActiveTouch();
  releasePressedStowawayKeys();
  clearGuestButtons();
  worker.postMessage({ type: "reset" });
  setStatus("Resetting emulator...");
});

clearSerialBtn.addEventListener("click", (event) => {
  // The button sits in the console's <summary>; don't toggle the drawer.
  event.preventDefault();
  event.stopPropagation();
  serialEl.textContent = "";
  consoleEl.classList.remove("has-new");
});

screenEl.addEventListener("pointerdown", (event) => {
  if (!running || activeTouchPointerId !== null) {
    return;
  }
  const point = mapPointerToGuest(event);
  activeTouchPointerId = event.pointerId;
  activeTouchPoint = point;
  screenEl.setPointerCapture(event.pointerId);
  worker.postMessage({ type: "setTouch", down: true, ...point });
});

screenEl.addEventListener("pointermove", (event) => {
  if (!running || event.pointerId !== activeTouchPointerId ||
      !screenEl.hasPointerCapture(event.pointerId)) {
    return;
  }
  const point = mapPointerToGuest(event);
  activeTouchPoint = point;
  worker.postMessage({ type: "setTouch", down: true, ...point });
});

function releaseTouch(event) {
  if (event.pointerId !== activeTouchPointerId) {
    return;
  }
  const point = event.type === "pointercancel" ?
    activeTouchPoint : mapPointerToGuest(event);
  activeTouchPointerId = null;
  if (screenEl.hasPointerCapture(event.pointerId)) {
    screenEl.releasePointerCapture(event.pointerId);
  }
  worker.postMessage({ type: "setTouch", down: false, ...point });
}

function cancelActiveTouch() {
  if (activeTouchPointerId === null) {
    return;
  }
  const pointerId = activeTouchPointerId;
  activeTouchPointerId = null;
  if (screenEl.hasPointerCapture(pointerId)) {
    screenEl.releasePointerCapture(pointerId);
  }
  worker.postMessage({ type: "setTouch", down: false, ...activeTouchPoint });
}

screenEl.addEventListener("pointerup", releaseTouch);
screenEl.addEventListener("pointercancel", releaseTouch);
screenEl.addEventListener("lostpointercapture", (event) => {
  if (event.pointerId === activeTouchPointerId) {
    cancelActiveTouch();
  }
});

function pressFrameButton(event) {
  if (!running || activeFrameButton || event.target === screenEl) {
    return;
  }
  const point = mapPointerToFrame(event);
  if (!point) {
    return;
  }
  const hit = hitFrameButton(point);
  if (!hit) {
    return;
  }

  event.preventDefault();
  deviceFrameEl.setPointerCapture(event.pointerId);
  activeFrameButton = { pointerId: event.pointerId, hit };
  pressMomentaryButton(hit.targetSet, hit.mask);
}

function releaseFrameButton(event) {
  if (!activeFrameButton || activeFrameButton.pointerId !== event.pointerId) {
    return;
  }

  event.preventDefault();
  if (deviceFrameEl.hasPointerCapture(event.pointerId)) {
    deviceFrameEl.releasePointerCapture(event.pointerId);
  }
  releaseMomentaryButton(activeFrameButton.hit.targetSet, activeFrameButton.hit.mask);
  activeFrameButton = null;
}

deviceFrameEl.addEventListener("pointerdown", pressFrameButton);
deviceFrameEl.addEventListener("pointerup", releaseFrameButton);
deviceFrameEl.addEventListener("pointercancel", releaseFrameButton);

function releasePressedDeviceKeys() {
  for (const mask of pressedDeviceKeys.values()) {
    releaseMomentaryButton("btn1", mask);
  }
  pressedDeviceKeys.clear();
}

window.addEventListener("keydown", (event) => {
  if (!running || activeTargusKeyboardEnabled || isEditableTarget(event.target)) {
    return;
  }
  const mask = DEVICE_KEY_MASKS.get(event.code);
  if (typeof mask !== "number") {
    return;
  }
  event.preventDefault();
  if (event.repeat || pressedDeviceKeys.has(event.code)) {
    return;
  }
  pressedDeviceKeys.set(event.code, mask);
  pressMomentaryButton("btn1", mask);
});

window.addEventListener("keyup", (event) => {
  const mask = pressedDeviceKeys.get(event.code);
  if (typeof mask !== "number") {
    return;
  }
  event.preventDefault();
  pressedDeviceKeys.delete(event.code);
  releaseMomentaryButton("btn1", mask);
});

window.addEventListener("blur", releasePressedDeviceKeys);

window.addEventListener("keydown", (event) => {
  if (!running || !activeTargusKeyboardEnabled || isEditableTarget(event.target)) {
    return;
  }
  const scancode = STOWAWAY_KEY_CODES.get(event.code);
  if (typeof scancode !== "number") {
    return;
  }
  event.preventDefault();
  if (event.repeat || pressedStowawayKeys.has(event.code)) {
    return;
  }
  pressedStowawayKeys.add(event.code);
  sendStowawayKey(scancode, false);
});

window.addEventListener("keyup", (event) => {
  if (!activeTargusKeyboardEnabled) {
    return;
  }
  const scancode = STOWAWAY_KEY_CODES.get(event.code);
  if (typeof scancode !== "number" || !pressedStowawayKeys.has(event.code)) {
    return;
  }
  event.preventDefault();
  pressedStowawayKeys.delete(event.code);
  sendStowawayKey(scancode, true);
});

window.addEventListener("blur", releasePressedStowawayKeys);

for (const button of controlButtons) {
  const targetSet = button.dataset.set;
  const mask = Number(button.dataset.mask);
  const isToggle = button.dataset.toggle === "true";

  if (isToggle) {
    button.addEventListener("click", (event) => {
      event.preventDefault();
      setButtonMask(targetSet, mask, !isMaskActive(targetSet, mask));
    });
    continue;
  }

  const press = (event) => {
    event.preventDefault();
    button.setPointerCapture(event.pointerId);
    pressMomentaryButton(targetSet, mask);
  };

  const release = (event) => {
    event.preventDefault();
    if (button.hasPointerCapture(event.pointerId)) {
      button.releasePointerCapture(event.pointerId);
    }
    releaseMomentaryButton(targetSet, mask);
  };

  button.addEventListener("pointerdown", press);
  button.addEventListener("pointerup", release);
  button.addEventListener("pointercancel", release);
  button.addEventListener("pointerleave", release);
}

worker.addEventListener("message", ({ data }) => {
  switch (data.type) {
    case "status":
      const wasRunning = running;
      if (typeof data.running === "boolean") {
        running = data.running;
      }
      if (typeof data.bootInFlight === "boolean") {
        bootInFlight = data.bootInFlight;
      }
      /*
       * Replacing or resetting a machine briefly reports running=false while
       * the worker stages the new image and reconnects networking.  Keep the
       * selected Stowaway backend across that transition; clearing it here
       * made only the first image booted by a page accept browser key events.
       * A genuine stop still has bootInFlight=false and follows this path.
       */
      if (wasRunning && !running && !bootInFlight) {
        releasePressedStowawayKeys();
        activeTargusKeyboardEnabled = false;
      }
      if (data.message) {
        setStatus(data.message);
      }
      syncButtons();
      break;
    case "frame":
      if (screenEl.width !== data.width || screenEl.height !== data.height) {
        screenEl.width = data.width;
        screenEl.height = data.height;
      }
      if (!imageData || imageData.width !== data.width || imageData.height !== data.height) {
        imageData = screenCtx.createImageData(data.width, data.height);
      }
      imageData.data.set(new Uint8ClampedArray(data.buffer));
      screenCtx.putImageData(imageData, 0, 0);
      break;
    case "serial":
      appendSerial(data.text);
      break;
    case "audio":
      playAudio(data.samples);
      break;
    case "cfImage":
      if (data.bytes) {
        downloadCfImage(data.bytes);
      } else {
        setStatus("No CompactFlash card is attached to download.");
      }
      break;
    case "fatal":
      running = false;
      bootInFlight = false;
      statusError = true;
      activeTargusKeyboardEnabled = false;
      releasePressedStowawayKeys();
      clearGuestButtons();
      setStatus(data.message);
      appendSerial(`\n[FATAL] ${data.message}\n`);
      syncButtons();
      break;
    default:
      break;
  }
});

const launch = parseLaunchConfig(window.location.search, HOSTED_NAND_IMAGES.map((image) => image.id));
const launchControls = {
  image: nandSelect, card: primarySocketSelect, cfSize: cfBlankSizeSelect,
  speed: speedControlInput, keyboard: targusKeyboardEnabledInput,
  audio: audioEnabledInput, mac: netMacInput, bridge: netBridgeUrlInput,
};
for (const [name, value] of Object.entries(launch.settings)) {
  if (typeof value === "boolean") launchControls[name].checked = value;
  else launchControls[name].value = value;
}
lastHostedNand = nandSelect.value;

syncButtons();
syncControlButtonStates();
syncSpeedLabel();
syncPrimarySocketControls();
syncNandControls();
renderCfBlankFiles();
renderPendingPackages();
loadFrameButtonMask();
updateReadyStatus();

if (launch.errors.length) {
  statusError = true;
  setStatus(launch.errors.join(" "));
  syncStatusState();
} else if (launch.boot) {
  void bootMachine(false);
}

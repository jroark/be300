const ASSET_VERSION = "20260429g";
const worker = new Worker(new URL(`./worker.js?v=${ASSET_VERSION}`, import.meta.url), {
  type: "module",
});

// The WinCE NAND boot only supports the stock 16 MB SDRAM layout. Speed 0
// means unthrottled/full speed; non-zero values are pacing units, not MHz.
const DEFAULT_SDRAM_MB = 16;
const DEFAULT_SPEED = 0;
const DEFAULT_NAND_URL = "./All_nand_300.bin";

const nandFileInput = document.querySelector("#nandFile");
const speedControlInput = document.querySelector("#speedControl");
const speedValueEl = document.querySelector("#speedValue");
const primarySocketSelect = document.querySelector("#primarySocket");
const cf0FileInput = document.querySelector("#cf0File");
const cf1FileInput = document.querySelector("#cf1File");
const netMacInput = document.querySelector("#netMac");
const netBridgeUrlInput = document.querySelector("#netBridgeUrl");
const targusKeyboardEnabledInput = document.querySelector("#targusKeyboardEnabled");
const bootBtn = document.querySelector("#bootBtn");
const stopBtn = document.querySelector("#stopBtn");
const resetBtn = document.querySelector("#resetBtn");
const toggleSerialBtn = document.querySelector("#toggleSerialBtn");
const clearSerialBtn = document.querySelector("#clearSerialBtn");
const statusEl = document.querySelector("#status");
const serialEl = document.querySelector("#serial");
const serialPanelEl = document.querySelector(".serial-panel");
const screenEl = document.querySelector("#screen");
const screenCtx = screenEl.getContext("2d", { alpha: false });
const controlButtons = Array.from(document.querySelectorAll("[data-set][data-mask]"));

let uploadedNandFile = null;
let running = false;
let bootInFlight = false;
let serialCollapsed = false;
let imageData = screenCtx.createImageData(screenEl.width, screenEl.height);
let btnSet1 = 0;
let btnSet2 = 0;
let activeTargusKeyboardEnabled = false;
const pressedStowawayKeys = new Set();

const STOWAWAY_KEY_CODES = new Map([
  ["Digit1", 0], ["Digit2", 1], ["Digit3", 2], ["KeyZ", 3],
  ["Digit4", 4], ["Digit5", 5], ["Digit6", 6], ["Digit7", 7],
  ["KeyQ", 9], ["KeyW", 10], ["KeyE", 11], ["KeyR", 12],
  ["KeyT", 13], ["KeyY", 14], ["Backquote", 15], ["KeyX", 16],
  ["KeyA", 17], ["KeyS", 18], ["KeyD", 19], ["KeyF", 20],
  ["KeyG", 21], ["KeyH", 22], ["Space", 23], ["CapsLock", 24],
  ["Tab", 25], ["ControlLeft", 26], ["ControlRight", 26],
  ["AltLeft", 35], ["AltRight", 35], ["KeyC", 44], ["KeyV", 45],
  ["KeyB", 46], ["KeyN", 47], ["Minus", 48], ["Equal", 49],
  ["Backspace", 50], ["Home", 51], ["Digit8", 52], ["Digit9", 53],
  ["Digit0", 54], ["Escape", 55], ["BracketLeft", 56],
  ["BracketRight", 57], ["Backslash", 58], ["End", 59],
  ["KeyU", 60], ["KeyI", 61], ["KeyO", 62], ["KeyP", 63],
  ["Quote", 64], ["Enter", 65], ["NumpadEnter", 65],
  ["PageUp", 66], ["KeyJ", 68], ["KeyK", 69], ["KeyL", 70],
  ["Semicolon", 71], ["Slash", 72], ["ArrowUp", 73],
  ["PageDown", 74], ["KeyM", 76], ["Comma", 77], ["Period", 78],
  ["Insert", 79], ["Delete", 80], ["ArrowLeft", 81],
  ["ArrowDown", 82], ["ArrowRight", 83], ["ShiftLeft", 87],
  ["ShiftRight", 88], ["F1", 105], ["F2", 106], ["F3", 107],
  ["F4", 108], ["F5", 109], ["F6", 110], ["F7", 111],
  ["F8", 112], ["F9", 113], ["F10", 114], ["F11", 115],
  ["F12", 116],
]);

function setStatus(text) {
  statusEl.textContent = text;
}

function getActiveNand() {
  if (uploadedNandFile) {
    return { kind: "upload", name: uploadedNandFile.name, file: uploadedNandFile };
  }
  return { kind: "fetch", name: "All_nand_300.bin", url: DEFAULT_NAND_URL };
}

function getActiveCf(slot) {
  if (slot === 0 && primarySocketSelect.value !== "cf0") {
    return null;
  }
  const input = slot === 0 ? cf0FileInput : cf1FileInput;
  const file = input?.files?.[0] ?? null;
  return file ? { name: file.name, file } : null;
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
  const usingNe2000 = primarySocket === "ne2000";

  netMacInput.disabled = !usingNe2000;
  netBridgeUrlInput.disabled = !usingNe2000;
}

function parseMacAddress(value) {
  const text = value.trim();
  if (!text) {
    return { valid: true, bytes: null };
  }

  const parts = text.split(":");
  if (parts.length !== 6) {
    return { valid: false, bytes: null };
  }

  const bytes = new Uint8Array(6);
  for (let i = 0; i < parts.length; i++) {
    if (!/^[0-9a-fA-F]{2}$/.test(parts[i])) {
      return { valid: false, bytes: null };
    }
    bytes[i] = Number.parseInt(parts[i], 16);
  }
  if ((bytes[0] & 1) !== 0) {
    return { valid: false, bytes: null };
  }
  return { valid: true, bytes };
}

function normalizeBridgeUrl(value) {
  const text = value.trim();
  if (!text) {
    return { valid: true, url: "" };
  }
  try {
    const url = new URL(text);
    if (url.protocol !== "ws:" && url.protocol !== "wss:") {
      return { valid: false, url: "" };
    }
    return { valid: true, url: url.href };
  } catch {
    return { valid: false, url: "" };
  }
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
    mac,
    bridge,
  };
}

function validateBootOptions(options = getBootOptions()) {
  if (options.primarySocket === "cf0" && !options.cf0) {
    return {
      valid: false,
      message: "Choose CompactFlash slot 0 again to select a CF image, or choose a different primary PCMCIA card.",
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
    parts.push(`CF0 ${options.cf0.name}`);
  }
  if (options.cf1) {
    parts.push(`CF1 ${options.cf1.name}`);
  }
  if (options.enableNe2000) {
    parts.push(options.bridge.url ? "NE2000 bridge" : "NE2000 internal net");
  }
  if (options.enableTargusKeyboard) {
    parts.push("Targus KB");
  }
  return parts;
}

function updateReadyStatus() {
  if (bootInFlight || running) {
    return;
  }
  const options = getBootOptions();
  const validation = validateBootOptions(options);
  if (!validation.valid) {
    setStatus(validation.message);
    return;
  }
  const active = getActiveNand();
  const accessories = describeAccessories(options);
  const suffix = accessories.length ? ` with ${accessories.join(", ")}` : "";
  setStatus(`Ready to boot ${active.name}${suffix} at ${formatSpeed(options.speed)}.`);
}

function syncButtons() {
  const validation = validateBootOptions();
  // We always have at least the default fetch URL available, so Boot is
  // enabled until we're actually mid-boot. If the fetch fails the worker
  // will surface the error.
  bootBtn.disabled = bootInFlight || !validation.valid;
  stopBtn.disabled = !running;
  resetBtn.disabled = bootInFlight;
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

function clearGuestButtons() {
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

function isEditableTarget(target) {
  if (!(target instanceof Element)) {
    return false;
  }
  const tagName = target.tagName.toLowerCase();
  return target.isContentEditable ||
    tagName === "input" ||
    tagName === "select" ||
    tagName === "textarea" ||
    tagName === "button";
}

function mapPointerToGuest(event) {
  const rect = screenEl.getBoundingClientRect();
  const x = clamp(Math.round(((event.clientX - rect.left) / rect.width) * 239), 0, 239);
  const y = clamp(Math.round(((event.clientY - rect.top) / rect.height) * 319), 0, 319);
  return { x, y };
}

function appendSerial(text) {
  serialEl.textContent += text;
  if (serialEl.textContent.length > 120000) {
    serialEl.textContent = serialEl.textContent.slice(-80000);
  }
  serialEl.scrollTop = serialEl.scrollHeight;
}

function setSerialCollapsed(collapsed) {
  serialCollapsed = collapsed;
  serialPanelEl.classList.toggle("collapsed", collapsed);
  toggleSerialBtn.textContent = collapsed ? "Expand" : "Collapse";
  toggleSerialBtn.setAttribute("aria-expanded", collapsed ? "false" : "true");
}

nandFileInput.addEventListener("change", () => {
  uploadedNandFile = nandFileInput.files?.[0] ?? null;
  updateReadyStatus();
  syncButtons();
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

cf0FileInput.addEventListener("change", () => {
  if (cf0FileInput.files?.[0]) {
    primarySocketSelect.value = "cf0";
  }
  syncPrimarySocketControls();
  updateReadyStatus();
  syncButtons();
});

bootBtn.addEventListener("click", async () => {
  const active = getActiveNand();
  const options = getBootOptions();
  const validation = validateBootOptions(options);
  if (!validation.valid) {
    setStatus(validation.message);
    syncButtons();
    return;
  }

  bootInFlight = true;
  running = false;
  activeTargusKeyboardEnabled = options.enableTargusKeyboard;
  releasePressedStowawayKeys();
  clearGuestButtons();
  serialEl.textContent = "";
  setStatus(`Booting ${active.name}...`);
  syncButtons();

  const message = {
    type: "bootNand",
    nandName: active.name,
    sdramMb: DEFAULT_SDRAM_MB,
    targetMhz: options.speed,
    enableNe2000: options.enableNe2000,
    enableTargusKeyboard: options.enableTargusKeyboard,
    netMac: options.mac.bytes ? Array.from(options.mac.bytes) : null,
    netBridgeUrl: options.enableNe2000 ? options.bridge.url : "",
    cfSlot0Name: options.cf0?.name || "",
    cfSlot1Name: options.cf1?.name || "",
  };
  const transfer = [];

  try {
    if (active.kind === "upload") {
      message.nandBytes = await active.file.arrayBuffer();
      transfer.push(message.nandBytes);
    } else {
      message.nandUrl = `${active.url}?v=${ASSET_VERSION}`;
    }
    if (options.cf0) {
      message.cfSlot0Bytes = await options.cf0.file.arrayBuffer();
      transfer.push(message.cfSlot0Bytes);
    }
    if (options.cf1) {
      message.cfSlot1Bytes = await options.cf1.file.arrayBuffer();
      transfer.push(message.cfSlot1Bytes);
    }
  } catch (error) {
    bootInFlight = false;
    const messageText = `Boot failed: ${error instanceof Error ? error.message : String(error)}`;
    setStatus(messageText);
    appendSerial(`\n[FATAL] ${messageText}\n`);
    syncButtons();
    return;
  }

  if (transfer.length) {
    worker.postMessage(message, transfer);
  } else {
    worker.postMessage(message);
  }
});

stopBtn.addEventListener("click", () => {
  releasePressedStowawayKeys();
  clearGuestButtons();
  worker.postMessage({ type: "stop" });
  setStatus("Stopping emulator...");
});

resetBtn.addEventListener("click", () => {
  releasePressedStowawayKeys();
  clearGuestButtons();
  worker.postMessage({ type: "reset" });
  setStatus("Resetting emulator...");
});

clearSerialBtn.addEventListener("click", () => {
  serialEl.textContent = "";
});

toggleSerialBtn.addEventListener("click", () => {
  setSerialCollapsed(!serialCollapsed);
});

screenEl.addEventListener("pointerdown", (event) => {
  if (!running) {
    return;
  }
  const point = mapPointerToGuest(event);
  screenEl.setPointerCapture(event.pointerId);
  worker.postMessage({ type: "setTouch", down: true, ...point });
});

screenEl.addEventListener("pointermove", (event) => {
  if (!running || !screenEl.hasPointerCapture(event.pointerId)) {
    return;
  }
  const point = mapPointerToGuest(event);
  worker.postMessage({ type: "setTouch", down: true, ...point });
});

function releaseTouch(event) {
  const point = mapPointerToGuest(event);
  if (screenEl.hasPointerCapture(event.pointerId)) {
    screenEl.releasePointerCapture(event.pointerId);
  }
  worker.postMessage({ type: "setTouch", down: false, ...point });
}

screenEl.addEventListener("pointerup", releaseTouch);
screenEl.addEventListener("pointercancel", releaseTouch);

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
    setButtonMask(targetSet, mask, true);
  };

  const release = (event) => {
    event.preventDefault();
    if (button.hasPointerCapture(event.pointerId)) {
      button.releasePointerCapture(event.pointerId);
    }
    setButtonMask(targetSet, mask, false);
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
      if (wasRunning && !running) {
        releasePressedStowawayKeys();
        activeTargusKeyboardEnabled = false;
      }
      if (data.message) {
        setStatus(data.message);
      }
      syncButtons();
      break;
    case "frame":
      if (!imageData || imageData.width !== data.width || imageData.height !== data.height) {
        imageData = screenCtx.createImageData(data.width, data.height);
      }
      imageData.data.set(new Uint8ClampedArray(data.buffer));
      screenCtx.putImageData(imageData, 0, 0);
      break;
    case "serial":
      appendSerial(data.text);
      break;
    case "fatal":
      running = false;
      bootInFlight = false;
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

syncButtons();
syncControlButtonStates();
syncSpeedLabel();
syncPrimarySocketControls();
setSerialCollapsed(false);
updateReadyStatus();

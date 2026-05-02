const ASSET_VERSION = "20260429i";
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
const deviceFrameEl = document.querySelector(".device-frame");
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
let frameButtonMask = null;
let activeFrameButton = null;
const activeMomentaryButtons = new Map();
const pendingMomentaryButtonReleases = new Map();

const FRAME_BUTTON_MASK_URL = `./buttons_dpad_bw_mask.png?v=${ASSET_VERSION}`;
const BUTTON_MASK_THRESHOLD = 128;
const BUTTON_MASK_MIN_AREA = 500;
const BUTTON_MIN_DWELL_MS = 120;

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
    tagName === "button";
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

function insertButtonComponent(components, component) {
  if (component.area < BUTTON_MASK_MIN_AREA) {
    return;
  }
  components.push(component);
  components.sort((a, b) => b.area - a.area);
  if (components.length > 16) {
    components.pop();
  }
}

function collectButtonMaskComponents(active, width, height) {
  const seen = new Uint8Array(active.length);
  const queue = new Uint32Array(active.length);
  const components = [];

  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const start = y * width + x;
      if (!active[start] || seen[start]) {
        continue;
      }

      let head = 0;
      let tail = 0;
      let area = 0;
      let minX = x;
      let maxX = x;
      let minY = y;
      let maxY = y;
      let sumX = 0;
      let sumY = 0;

      seen[start] = 1;
      queue[tail++] = start;

      while (head < tail) {
        const p = queue[head++];
        const px = p % width;
        const py = Math.floor(p / width);
        const y0 = py > 0 ? py - 1 : py;
        const y1 = py + 1 < height ? py + 1 : py;
        const x0 = px > 0 ? px - 1 : px;
        const x1 = px + 1 < width ? px + 1 : px;

        area++;
        sumX += px;
        sumY += py;
        minX = Math.min(minX, px);
        maxX = Math.max(maxX, px);
        minY = Math.min(minY, py);
        maxY = Math.max(maxY, py);

        for (let ny = y0; ny <= y1; ny++) {
          for (let nx = x0; nx <= x1; nx++) {
            if (nx === px && ny === py) {
              continue;
            }
            const np = ny * width + nx;
            if (!active[np] || seen[np]) {
              continue;
            }
            seen[np] = 1;
            queue[tail++] = np;
          }
        }
      }

      insertButtonComponent(components, {
        area,
        minX,
        minY,
        maxX,
        maxY,
        cx: sumX / area,
        cy: sumY / area,
      });
    }
  }

  return components;
}

function splitSideComponent(active, width, component, leftSide) {
  const boxW = component.maxX - component.minX + 1;
  const boxH = component.maxY - component.minY + 1;
  let upperX = component.minX + (leftSide ? 0.25 : 0.75) * boxW;
  let upperY = component.minY + 0.30 * boxH;
  let lowerX = component.minX + (leftSide ? 0.75 : 0.25) * boxW;
  let lowerY = component.minY + 0.82 * boxH;

  for (let iter = 0; iter < 8; iter++) {
    let upperSumX = 0;
    let upperSumY = 0;
    let lowerSumX = 0;
    let lowerSumY = 0;
    let upperCount = 0;
    let lowerCount = 0;

    for (let y = component.minY; y <= component.maxY; y++) {
      for (let x = component.minX; x <= component.maxX; x++) {
        if (!active[y * width + x]) {
          continue;
        }
        const du = ((x - upperX) ** 2) + ((y - upperY) ** 2);
        const dl = ((x - lowerX) ** 2) + ((y - lowerY) ** 2);
        if (du <= dl) {
          upperSumX += x;
          upperSumY += y;
          upperCount++;
        } else {
          lowerSumX += x;
          lowerSumY += y;
          lowerCount++;
        }
      }
    }

    if (upperCount) {
      upperX = upperSumX / upperCount;
      upperY = upperSumY / upperCount;
    }
    if (lowerCount) {
      lowerX = lowerSumX / lowerCount;
      lowerY = lowerSumY / lowerCount;
    }
  }

  return {
    upper: { ...component, cx: upperX, cy: upperY },
    lower: { ...component, cx: lowerX, cy: lowerY },
  };
}

function assignButtonMaskRegions(components, active, width) {
  if (components.length < 3) {
    return null;
  }

  const dpad = components[0];
  if (components.length < 5) {
    let leftSide = null;
    let rightSide = null;
    for (const component of components.slice(1)) {
      if (component.cx < dpad.cx) {
        if (!leftSide || component.area > leftSide.area) {
          leftSide = component;
        }
      } else if (!rightSide || component.area > rightSide.area) {
        rightSide = component;
      }
    }
    if (!leftSide || !rightSide) {
      return null;
    }

    const left = splitSideComponent(active, width, leftSide, true);
    const right = splitSideComponent(active, width, rightSide, false);
    return {
      dpad,
      rocket: left.upper,
      ok: left.lower,
      power: right.upper,
      esc: right.lower,
    };
  }

  const left = [];
  const right = [];
  for (const component of components.slice(1)) {
    (component.cx < dpad.cx ? left : right).push(component);
  }
  if (left.length < 2 || right.length < 2) {
    return null;
  }

  left.sort((a, b) => a.cy - b.cy);
  right.sort((a, b) => a.cy - b.cy);
  return {
    dpad,
    rocket: left[0],
    ok: left[left.length - 1],
    power: right[0],
    esc: right[right.length - 1],
  };
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
      const regions = assignButtonMaskRegions(
        collectButtonMaskComponents(active, width, height),
        active,
        width,
      );
      if (!regions) {
        console.warn("BE-300 button mask did not contain the expected button regions.");
        return;
      }
      frameButtonMask = { active, width, height, regions };
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
  requestStop();
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
loadFrameButtonMask();
updateReadyStatus();

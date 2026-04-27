const ASSET_VERSION = "20260426a";
const worker = new Worker(new URL(`./worker.js?v=${ASSET_VERSION}`, import.meta.url), {
  type: "module",
});

// Reasonable defaults baked in. The historical Linux UI exposed SDRAM /
// speed sliders; the WinCE NAND boot only supports the stock 16 MB SDRAM
// layout, and 15 MHz is the same target the prior Linux build defaulted to.
const DEFAULT_SDRAM_MB = 16;
const DEFAULT_TARGET_MHZ = 15;
const DEFAULT_NAND_URL = "./All_nand_300.bin";

const nandFileInput = document.querySelector("#nandFile");
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

function setStatus(text) {
  statusEl.textContent = text;
}

function getActiveNand() {
  if (uploadedNandFile) {
    return { kind: "upload", name: uploadedNandFile.name, file: uploadedNandFile };
  }
  return { kind: "fetch", name: "All_nand_300.bin", url: DEFAULT_NAND_URL };
}

function updateNandStatus() {
  const active = getActiveNand();
  setStatus(`Ready to boot ${active.name}.`);
}

function syncButtons() {
  // We always have at least the default fetch URL available, so Boot is
  // enabled until we're actually mid-boot. If the fetch fails the worker
  // will surface the error.
  bootBtn.disabled = bootInFlight;
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
  updateNandStatus();
  syncButtons();
});

bootBtn.addEventListener("click", async () => {
  const active = getActiveNand();
  bootInFlight = true;
  running = false;
  clearGuestButtons();
  serialEl.textContent = "";
  setStatus(`Booting ${active.name}...`);
  syncButtons();

  const message = {
    type: "bootNand",
    nandName: active.name,
    sdramMb: DEFAULT_SDRAM_MB,
    targetMhz: DEFAULT_TARGET_MHZ,
  };

  try {
    if (active.kind === "upload") {
      message.nandBytes = await active.file.arrayBuffer();
    } else {
      message.nandUrl = `${active.url}?v=${ASSET_VERSION}`;
    }
  } catch (error) {
    bootInFlight = false;
    const messageText = `Boot failed: ${error instanceof Error ? error.message : String(error)}`;
    setStatus(messageText);
    appendSerial(`\n[FATAL] ${messageText}\n`);
    syncButtons();
    return;
  }

  if (message.nandBytes) {
    worker.postMessage(message, [message.nandBytes]);
  } else {
    worker.postMessage(message);
  }
});

stopBtn.addEventListener("click", () => {
  clearGuestButtons();
  worker.postMessage({ type: "stop" });
  setStatus("Stopping emulator...");
});

resetBtn.addEventListener("click", () => {
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
      if (typeof data.running === "boolean") {
        running = data.running;
      }
      if (typeof data.bootInFlight === "boolean") {
        bootInFlight = data.bootInFlight;
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
setSerialCollapsed(false);
updateNandStatus();

const ASSET_VERSION = "20260329a";
const worker = new Worker(new URL(`./worker.js?v=${ASSET_VERSION}`, import.meta.url), {
  type: "module",
});

const presetKernelSelect = document.querySelector("#presetKernel");
const kernelFileInput = document.querySelector("#kernelFile");
const cmdlineInput = document.querySelector("#cmdline");
const sdramInput = document.querySelector("#sdramMb");
const speedSlider = document.querySelector("#speedSlider");
const speedValue = document.querySelector("#speedValue");
const sfb5BitGreenInput = document.querySelector("#sfb5BitGreen");
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

const CUSTOM_UPLOAD_ID = "custom-upload";
const presetKernels = {
  "vmlinux-pgui-demo": {
    name: "vmlinux-pgui-demo",
    url: "./kernels/vmlinux-pgui-demo",
    defaultCmdline: "console=tty0 console=ttyS0,9600 root=/dev/ram",
  },
  "vmlinux-mw": {
    name: "vmlinux-mw",
    url: "./kernels/vmlinux-mw",
    defaultCmdline: "console=tty0 root=/dev/ram",
  },
  "vmlinux-2.6": {
    name: "vmlinux-2.6",
    url: "./kernels/vmlinux-2.6",
    defaultCmdline: "",
  },
  "vmlinux-4.2.9": {
    name: "vmlinux-4.2.9",
    url: "./kernels/vmlinux-4.2.9",
    defaultCmdline: "console=tty0 console=ttyS0,9600 earlyprintk keep_bootcon root=/dev/ram",
  },
};

let uploadedKernelFile = null;
let running = false;
let bootInFlight = false;
let serialCollapsed = false;
let imageData = screenCtx.createImageData(screenEl.width, screenEl.height);
let btnSet1 = 0;
let btnSet2 = 0;

function clampInteger(value, min, max, fallback) {
  const parsed = Number.parseInt(value, 10);
  if (!Number.isFinite(parsed)) {
    return fallback;
  }
  return Math.min(max, Math.max(min, parsed));
}

function sliderToSpeed(sliderVal) {
  const v = clampInteger(sliderVal, 1, 100, 10);
  if (v >= 100) return 166;
  return Math.round(1 + (v - 1) * (165 / 99));
}

function getBootConfig() {
  return {
    sdramMb: clampInteger(sdramInput.value, 1, 64, 16),
    targetMhz: sliderToSpeed(speedSlider.value),
    sfb5BitGreen: sfb5BitGreenInput.checked,
  };
}

function withManagedMemoryArg(cmdline, sdramMb) {
  const tokens = String(cmdline ?? "")
    .trim()
    .split(/\s+/)
    .filter((token) => token.length > 0 && !token.startsWith("mem="));
  if (sdramMb !== 16) {
    tokens.push(`mem=${sdramMb}M`);
  }
  return tokens.join(" ");
}

function syncCmdlineMemoryArg() {
  const { sdramMb } = getBootConfig();
  cmdlineInput.value = withManagedMemoryArg(cmdlineInput.value, sdramMb);
}

function setStatus(text) {
  statusEl.textContent = text;
}

function getActiveKernel() {
  if (presetKernelSelect.value === CUSTOM_UPLOAD_ID) {
    return uploadedKernelFile
      ? {
          kind: "upload",
          name: uploadedKernelFile.name,
          file: uploadedKernelFile,
        }
      : null;
  }

  const preset = presetKernels[presetKernelSelect.value];
  return preset ? { kind: "preset", ...preset } : null;
}

function updateKernelStatus() {
  const activeKernel = getActiveKernel();

  if (!activeKernel) {
    setStatus("Select a kernel or upload a custom kernel.");
    return;
  }

  setStatus(`Ready to boot ${activeKernel.name}.`);
}

function syncButtons() {
  const hasKernel = Boolean(getActiveKernel());
  const bootable = hasKernel && !bootInFlight;

  bootBtn.disabled = !bootable;
  stopBtn.disabled = !running;
  resetBtn.disabled = !hasKernel || bootInFlight;
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

presetKernelSelect.addEventListener("change", () => {
  if (presetKernelSelect.value === CUSTOM_UPLOAD_ID) {
    updateKernelStatus();
    syncButtons();
    return;
  }

  uploadedKernelFile = null;
  kernelFileInput.value = "";
  cmdlineInput.value = withManagedMemoryArg(
    presetKernels[presetKernelSelect.value].defaultCmdline,
    getBootConfig().sdramMb,
  );
  updateKernelStatus();
  syncButtons();
});

kernelFileInput.addEventListener("change", () => {
  uploadedKernelFile = kernelFileInput.files?.[0] ?? null;
  if (uploadedKernelFile) {
    presetKernelSelect.value = CUSTOM_UPLOAD_ID;
  }

  updateKernelStatus();
  syncButtons();
});

cmdlineInput.addEventListener("input", () => {
  syncButtons();
});

sdramInput.addEventListener("change", () => {
  syncCmdlineMemoryArg();
});

speedSlider.addEventListener("input", () => {
  speedValue.textContent = speedSlider.value;
});

bootBtn.addEventListener("click", async () => {
  const activeKernel = getActiveKernel();

  if (!activeKernel) {
    return;
  }

  bootInFlight = true;
  running = false;
  clearGuestButtons();
  serialEl.textContent = "";
  setStatus(`Booting ${activeKernel.name}...`);
  syncButtons();
  syncCmdlineMemoryArg();
  const message = {
    type: "bootLinux",
    kernelName: activeKernel.name,
    cmdline: cmdlineInput.value,
    ...getBootConfig(),
  };

  try {
    if (activeKernel.kind === "upload") {
      message.kernelBytes = await activeKernel.file.arrayBuffer();
    } else {
      message.kernelUrl = `${activeKernel.url}?v=${ASSET_VERSION}`;
    }
  } catch (error) {
    bootInFlight = false;
    const messageText = `Boot failed: ${error instanceof Error ? error.message : String(error)}`;
    setStatus(messageText);
    appendSerial(`\n[FATAL] ${messageText}\n`);
    syncButtons();
    return;
  }

  if (message.kernelBytes) {
    worker.postMessage(message, [message.kernelBytes]);
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
speedValue.textContent = speedSlider.value;
updateKernelStatus();

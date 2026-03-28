const worker = new Worker(new URL("./worker.js", import.meta.url), { type: "module" });

const kernelFileInput = document.querySelector("#kernelFile");
const cmdlineInput = document.querySelector("#cmdline");
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

let kernelFile = null;
let running = false;
let bootInFlight = false;
let serialCollapsed = false;
let imageData = screenCtx.createImageData(screenEl.width, screenEl.height);
let btnSet1 = 0;
let btnSet2 = 0;

function setStatus(text) {
  statusEl.textContent = text;
}

function syncButtons() {
  const hasKernel = Boolean(kernelFile);
  const hasCmdline = cmdlineInput.value.trim().length > 0;
  const bootable = hasKernel && hasCmdline && !bootInFlight;

  bootBtn.disabled = !bootable;
  stopBtn.disabled = !running;
  resetBtn.disabled = !hasKernel || !hasCmdline || bootInFlight;
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

kernelFileInput.addEventListener("change", () => {
  kernelFile = kernelFileInput.files?.[0] ?? null;
  syncButtons();
  if (kernelFile) {
    setStatus(`Ready to boot ${kernelFile.name}.`);
  } else {
    setStatus("Waiting for kernel upload.");
  }
});

cmdlineInput.addEventListener("input", () => {
  syncButtons();
});

bootBtn.addEventListener("click", () => {
  if (!kernelFile || !cmdlineInput.value.trim()) {
    return;
  }
  bootInFlight = true;
  running = false;
  serialEl.textContent = "";
  setStatus(`Booting ${kernelFile.name}...`);
  syncButtons();
  worker.postMessage({
    type: "bootLinux",
    kernelFile,
    cmdline: cmdlineInput.value.trim(),
  });
});

stopBtn.addEventListener("click", () => {
  worker.postMessage({ type: "stop" });
  setStatus("Stopping emulator...");
});

resetBtn.addEventListener("click", () => {
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

for (const button of document.querySelectorAll("[data-set][data-mask]")) {
  const targetSet = button.dataset.set;
  const mask = Number(button.dataset.mask);

  const press = (event) => {
    event.preventDefault();
    button.setPointerCapture(event.pointerId);
    if (targetSet === "btn1") {
      btnSet1 |= mask;
    } else {
      btnSet2 |= mask;
    }
    sendButtons();
  };

  const release = (event) => {
    event.preventDefault();
    if (button.hasPointerCapture(event.pointerId)) {
      button.releasePointerCapture(event.pointerId);
    }
    if (targetSet === "btn1") {
      btnSet1 &= ~mask;
    } else {
      btnSet2 &= ~mask;
    }
    sendButtons();
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
      setStatus(data.message);
      appendSerial(`\n[FATAL] ${data.message}\n`);
      syncButtons();
      break;
    default:
      break;
  }
});

syncButtons();
setSerialCollapsed(false);

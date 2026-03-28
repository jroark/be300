import createBe300Module from "./be300_web.js?v=20260328b";

const FRAME_WIDTH = 240;
const FRAME_HEIGHT = 320;
const FRAME_BYTES = FRAME_WIDTH * FRAME_HEIGHT * 4;
const SERIAL_BYTES = 16384;
const BATCHES_PER_TICK = 64;
const FRAME_INTERVAL_MS = 33;

const textEncoder = new TextEncoder();
const textDecoder = new TextDecoder();

let moduleInstance = null;
let modulePromise = null;
let machineHandle = 0;
let running = false;
let tickScheduled = false;
let framePtr = 0;
let serialPtr = 0;
let widthPtr = 0;
let heightPtr = 0;
let cachedKernel = null;
let cachedCmdline = null;
let lastFrameAt = 0;

function postStatus(message, extra = {}) {
  postMessage({
    type: "status",
    message,
    running,
    ...extra,
  });
}

async function ensureModule() {
  if (!modulePromise) {
    modulePromise = createBe300Module();
  }
  if (!moduleInstance) {
    moduleInstance = await modulePromise;
  }
  return moduleInstance;
}

function ensureScratch(module) {
  if (!framePtr) {
    framePtr = module._malloc(FRAME_BYTES);
  }
  if (!serialPtr) {
    serialPtr = module._malloc(SERIAL_BYTES);
  }
  if (!widthPtr) {
    widthPtr = module._malloc(4);
  }
  if (!heightPtr) {
    heightPtr = module._malloc(4);
  }
}

function allocBytes(module, bytes) {
  const ptr = module._malloc(bytes.length);
  module.HEAPU8.set(bytes, ptr);
  return ptr;
}

function allocCString(module, text) {
  const bytes = textEncoder.encode(text);
  const ptr = module._malloc(bytes.length + 1);
  module.HEAPU8.set(bytes, ptr);
  module.HEAPU8[ptr + bytes.length] = 0;
  return ptr;
}

function destroyMachine() {
  if (moduleInstance && machineHandle) {
    moduleInstance._be300_destroy(machineHandle);
  }
  machineHandle = 0;
  running = false;
  tickScheduled = false;
}

function drainSerial(module) {
  if (!machineHandle) {
    return;
  }
  const count = module._be300_drain_serial(machineHandle, serialPtr, SERIAL_BYTES);
  if (count > 0) {
    const text = textDecoder.decode(module.HEAPU8.slice(serialPtr, serialPtr + count));
    postMessage({ type: "serial", text });
  }
}

function drainFrame(module) {
  const now = performance.now();
  if (!machineHandle || now - lastFrameAt < FRAME_INTERVAL_MS) {
    return;
  }

  const copied = module._be300_copy_frame_rgba8888(
    machineHandle,
    framePtr,
    FRAME_BYTES,
    widthPtr,
    heightPtr,
  );
  if (copied > 0) {
    const width = module.HEAPU32[widthPtr >> 2];
    const height = module.HEAPU32[heightPtr >> 2];
    const frame = module.HEAPU8.slice(framePtr, framePtr + width * height * 4);
    lastFrameAt = now;
    postMessage({ type: "frame", width, height, buffer: frame.buffer }, [frame.buffer]);
  }
}

function scheduleTick() {
  if (!running || tickScheduled) {
    return;
  }
  tickScheduled = true;
  setTimeout(runTick, 0);
}

function runTick() {
  tickScheduled = false;
  if (!running || !moduleInstance || !machineHandle) {
    return;
  }

  const rc = moduleInstance._be300_step(machineHandle, BATCHES_PER_TICK);
  drainSerial(moduleInstance);
  drainFrame(moduleInstance);

  if (rc > 0) {
    scheduleTick();
    return;
  }

  running = false;
  postStatus("Emulator stopped.");
}

async function bootKernel(kernelBytes, cmdline) {
  const module = await ensureModule();
  destroyMachine();
  ensureScratch(module);

  machineHandle = module._be300_create_web(16, 0, 0);
  if (!machineHandle) {
    postMessage({ type: "fatal", message: "Failed to create BE-300 machine." });
    return;
  }

  const kernelPtr = allocBytes(module, kernelBytes);
  const cmdlinePtr = allocCString(module, cmdline);
  const rc = module._be300_boot_linux_from_memory(
    machineHandle,
    kernelPtr,
    kernelBytes.length,
    cmdlinePtr,
  );
  module._free(kernelPtr);
  module._free(cmdlinePtr);

  if (rc !== 0) {
    destroyMachine();
    postMessage({ type: "fatal", message: "Kernel boot preparation failed." });
    return;
  }

  cachedKernel = kernelBytes;
  cachedCmdline = cmdline;
  running = true;
  lastFrameAt = 0;
  postStatus("Kernel loaded. Starting emulation...", { bootInFlight: false });
  scheduleTick();
}

async function loadKernelBytes(data) {
  if (data.kernelBytes) {
    return new Uint8Array(data.kernelBytes);
  }

  if (data.kernelFile) {
    if (typeof data.kernelFile.arrayBuffer !== "function") {
      throw new Error("Uploaded kernel could not be read. Reload the page and try again.");
    }
    return new Uint8Array(await data.kernelFile.arrayBuffer());
  }

  if (data.kernelUrl) {
    const response = await fetch(data.kernelUrl);
    if (!response.ok) {
      throw new Error(`Failed to fetch ${data.kernelName || "kernel"} (${response.status})`);
    }
    return new Uint8Array(await response.arrayBuffer());
  }

  throw new Error("No kernel selected.");
}

self.addEventListener("message", async ({ data }) => {
  switch (data.type) {
    case "bootLinux": {
      try {
        postStatus("Loading kernel image...", { bootInFlight: true });
        const kernelBytes = await loadKernelBytes(data);
        await bootKernel(kernelBytes, data.cmdline);
      } catch (error) {
        destroyMachine();
        postMessage({
          type: "fatal",
          message: `Boot failed: ${error instanceof Error ? error.message : String(error)}`,
        });
      }
      break;
    }
    case "reset":
      if (cachedKernel && cachedCmdline !== null) {
        postStatus("Resetting emulator...", { bootInFlight: true });
        await bootKernel(cachedKernel, cachedCmdline);
      }
      break;
    case "stop":
      if (moduleInstance && machineHandle) {
        moduleInstance._be300_stop(machineHandle);
        postStatus("Stop requested...");
      }
      break;
    case "setTouch":
      if (moduleInstance && machineHandle) {
        moduleInstance._be300_set_touch(machineHandle, data.down ? 1 : 0, data.x, data.y);
      }
      break;
    case "setButtons":
      if (moduleInstance && machineHandle) {
        moduleInstance._be300_set_buttons(machineHandle, data.btnSet1, data.btnSet2);
      }
      break;
    default:
      break;
  }
});

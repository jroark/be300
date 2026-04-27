import createBe300Module from "./be300_web.js?v=20260426a";

const FRAME_WIDTH = 240;
const FRAME_HEIGHT = 320;
const FRAME_BYTES = FRAME_WIDTH * FRAME_HEIGHT * 4;
const SERIAL_BYTES = 16384;
const DEFAULT_BATCHES_PER_TICK = 15;
const UNTHROTTLED_BATCHES_PER_TICK = 64;
const FRAME_INTERVAL_MS = 33;

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
let cachedNand = null;
let cachedBootConfig = null;
let lastFrameAt = 0;
let batchesPerTick = DEFAULT_BATCHES_PER_TICK;

function normalizeBootConfig(config = {}) {
  const parsedSdram = Number.parseInt(config.sdramMb, 10);
  const parsedSpeed = Number.parseInt(config.targetMhz, 10);

  return {
    sdramMb: Number.isFinite(parsedSdram) ? Math.min(64, Math.max(1, parsedSdram)) : 16,
    targetMhz: Number.isFinite(parsedSpeed) ? Math.min(1000, Math.max(0, parsedSpeed)) : 15,
  };
}

function getBatchesPerTick(targetMhz) {
  return targetMhz > 0 ? Math.max(1, targetMhz) : UNTHROTTLED_BATCHES_PER_TICK;
}

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

  const rc = moduleInstance._be300_step(machineHandle, batchesPerTick);
  drainSerial(moduleInstance);
  drainFrame(moduleInstance);

  if (rc > 0) {
    scheduleTick();
    return;
  }

  running = false;
  postStatus("Emulator stopped.");
}

async function bootNand(nandBytes, bootConfig) {
  const module = await ensureModule();
  const config = normalizeBootConfig(bootConfig);
  destroyMachine();
  ensureScratch(module);

  // Stage the NAND image bytes into MEMFS at the path our glue uses for
  // cfg.nand_path, then ask the existing public be300_create() to mount it.
  const nandPtr = allocBytes(module, nandBytes);
  const loadRc = module._be300_web_load_nand(nandPtr, nandBytes.length);
  module._free(nandPtr);

  if (loadRc !== 0) {
    postMessage({ type: "fatal", message: "Failed to stage NAND image into MEMFS." });
    return;
  }

  machineHandle = module._be300_web_create(config.sdramMb, config.targetMhz);
  if (!machineHandle) {
    postMessage({ type: "fatal", message: "Failed to create BE-300 machine." });
    return;
  }

  cachedNand = nandBytes;
  cachedBootConfig = config;
  batchesPerTick = getBatchesPerTick(config.targetMhz);
  running = true;
  lastFrameAt = 0;
  postStatus("NAND loaded. Starting emulation...", { bootInFlight: false });
  scheduleTick();
}

async function loadNandBytes(data) {
  if (data.nandBytes) {
    return new Uint8Array(data.nandBytes);
  }

  if (data.nandFile) {
    if (typeof data.nandFile.arrayBuffer !== "function") {
      throw new Error("Uploaded NAND image could not be read. Reload the page and try again.");
    }
    return new Uint8Array(await data.nandFile.arrayBuffer());
  }

  if (data.nandUrl) {
    const response = await fetch(data.nandUrl);
    if (!response.ok) {
      throw new Error(`Failed to fetch ${data.nandName || "NAND image"} (${response.status})`);
    }
    return new Uint8Array(await response.arrayBuffer());
  }

  throw new Error("No NAND image selected.");
}

self.addEventListener("message", async ({ data }) => {
  switch (data.type) {
    case "bootNand": {
      try {
        postStatus("Loading NAND image...", { bootInFlight: true });
        const nandBytes = await loadNandBytes(data);
        await bootNand(nandBytes, data);
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
      if (cachedNand && cachedBootConfig) {
        postStatus("Resetting emulator...", { bootInFlight: true });
        await bootNand(cachedNand, cachedBootConfig);
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

import createBe300Module from "./be300_web.js?v=20260511a";

const MAX_FRAME_WIDTH = 480;
const MAX_FRAME_HEIGHT = 640;
const MAX_FRAME_BYTES = MAX_FRAME_WIDTH * MAX_FRAME_HEIGHT * 4;
const SERIAL_BYTES = 16384;
const NET_FRAME_BYTES = 2048;
const DEFAULT_SPEED = 0;
const FULL_SPEED_BATCHES_PER_STEP = 128;
const FULL_SPEED_SLICE_MS = 12;
const FRAME_INTERVAL_MS = 33;

const WEB_FLAG_NE2000 = 0x00000001;
const WEB_FLAG_TARGUS_KEYBOARD = 0x00000002;
const WEB_FLAG_CF0 = 0x00000004;
const WEB_FLAG_CF1 = 0x00000008;
const WEB_FLAG_NET_MAC = 0x00000010;

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
let netTxPtr = 0;
let netRxPtr = 0;
let cachedNand = null;
let cachedCfSlot0 = null;
let cachedCfSlot1 = null;
let cachedBootConfig = null;
let lastFrameAt = 0;
let batchesPerTick = getBatchesPerTick(DEFAULT_SPEED);
let currentSpeed = DEFAULT_SPEED;
let netSocket = null;
let netBridgeGeneration = 0;

const scheduleFastTick = (() => {
  if (typeof MessageChannel === "function") {
    const channel = new MessageChannel();
    channel.port1.onmessage = () => runTick();
    return () => channel.port2.postMessage(0);
  }
  return () => setTimeout(runTick, 0);
})();

function normalizeBootConfig(config = {}) {
  const parsedSdram = Number.parseInt(config.sdramMb, 10);

  return {
    sdramMb: Number.isFinite(parsedSdram) ? Math.min(64, Math.max(1, parsedSdram)) : 16,
    targetMhz: normalizeSpeed(config.targetMhz),
    fbSize: config.fbSize === "480x640" ? "480x640" : "240x320",
    enableNe2000: config.enableNe2000 === true,
    enableTargusKeyboard: config.enableTargusKeyboard === true,
    netMac: Array.isArray(config.netMac) && config.netMac.length === 6
      ? Uint8Array.from(config.netMac)
      : null,
    netBridgeUrl: typeof config.netBridgeUrl === "string" ? config.netBridgeUrl.trim() : "",
  };
}

function normalizeSpeed(value) {
  const parsedSpeed = Number.parseInt(value, 10);
  return Number.isFinite(parsedSpeed) ? Math.min(64, Math.max(0, parsedSpeed)) : DEFAULT_SPEED;
}

function getBatchesPerTick(targetMhz) {
  return targetMhz > 0 ? Math.max(1, targetMhz) : FULL_SPEED_BATCHES_PER_STEP;
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
    framePtr = module._malloc(MAX_FRAME_BYTES);
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

function ensureNetScratch(module) {
  if (!netTxPtr) {
    netTxPtr = module._malloc(NET_FRAME_BYTES);
  }
  if (!netRxPtr) {
    netRxPtr = module._malloc(NET_FRAME_BYTES);
  }
}

function allocBytes(module, bytes) {
  const ptr = module._malloc(bytes.length);
  module.HEAPU8.set(bytes, ptr);
  return ptr;
}

function setNetBridgeEnabled(enabled) {
  if (moduleInstance && typeof moduleInstance._be300_web_set_net_bridge_enabled === "function") {
    moduleInstance._be300_web_set_net_bridge_enabled(enabled ? 1 : 0);
  }
}

function closeNetBridge(invalidate = true) {
  if (invalidate) {
    netBridgeGeneration++;
  }
  setNetBridgeEnabled(false);
  if (!netSocket) {
    return;
  }
  const socket = netSocket;
  netSocket = null;
  socket.onopen = null;
  socket.onerror = null;
  socket.onclose = null;
  socket.onmessage = null;
  if (socket.readyState === WebSocket.CONNECTING ||
      socket.readyState === WebSocket.OPEN) {
    socket.close();
  }
}

function postSerial(text) {
  postMessage({ type: "serial", text });
}

function injectNetFrame(bytes) {
  if (!moduleInstance || !machineHandle || !bytes || bytes.length < 14 ||
      bytes.length > NET_FRAME_BYTES) {
    return;
  }
  ensureNetScratch(moduleInstance);
  moduleInstance.HEAPU8.set(bytes, netRxPtr);
  moduleInstance._be300_web_net_rx(machineHandle, netRxPtr, bytes.length);
}

function drainNetTx(module) {
  if (!machineHandle || !netSocket ||
      netSocket.readyState !== WebSocket.OPEN) {
    return;
  }

  ensureNetScratch(module);
  for (let i = 0; i < 16; i++) {
    const len = module._be300_web_net_tx_pop(netTxPtr, NET_FRAME_BYTES);
    if (len <= 0) {
      return;
    }
    const frame = module.HEAPU8.slice(netTxPtr, netTxPtr + len);
    netSocket.send(frame.buffer);
  }
}

function connectNetBridge(url, generation) {
  if (!url) {
    return Promise.resolve();
  }

  return new Promise((resolve, reject) => {
    const socket = new WebSocket(url);
    let settled = false;
    const isCurrent = () => generation === netBridgeGeneration &&
      netSocket === socket;
    const timeout = setTimeout(() => {
      if (settled || !isCurrent()) {
        return;
      }
      settled = true;
      closeNetBridge(false);
      reject(new Error(`Timed out connecting to network bridge ${url}`));
    }, 3000);

    socket.binaryType = "arraybuffer";
    netSocket = socket;

    socket.onopen = () => {
      if (settled || !isCurrent()) {
        return;
      }
      settled = true;
      clearTimeout(timeout);
      setNetBridgeEnabled(true);
      postSerial(`[NET] Bridge connected: ${url}\n`);
      resolve();
    };
    socket.onerror = () => {
      if (settled || !isCurrent()) {
        return;
      }
      settled = true;
      clearTimeout(timeout);
      closeNetBridge(false);
      reject(new Error(`Failed to connect to network bridge ${url}`));
    };
    socket.onclose = () => {
      if (!isCurrent()) {
        return;
      }
      if (!settled) {
        settled = true;
        clearTimeout(timeout);
        reject(new Error(`Network bridge closed before connection: ${url}`));
        return;
      }
      netSocket = null;
      setNetBridgeEnabled(false);
      postSerial(`[NET] Bridge disconnected: ${url}; using internal gateway.\n`);
      postStatus("Network bridge disconnected; NE2000 internal gateway is active.");
    };
    socket.onmessage = async (event) => {
      let bytes;
      if (event.data instanceof ArrayBuffer) {
        bytes = new Uint8Array(event.data);
      } else if (event.data instanceof Blob) {
        bytes = new Uint8Array(await event.data.arrayBuffer());
      } else {
        return;
      }
      injectNetFrame(bytes);
    };
  });
}

function startNetBridge(url) {
  closeNetBridge();
  const generation = netBridgeGeneration;
  postSerial(`[NET] Connecting bridge in background: ${url}\n`);
  void connectNetBridge(url, generation).catch((error) => {
    if (generation !== netBridgeGeneration) {
      return;
    }
    postSerial(`[NET] ${error instanceof Error ? error.message : String(error)}; using internal gateway.\n`);
    postStatus("Network bridge unavailable; NE2000 internal gateway is active.", { bootInFlight: false });
  });
}

async function prepareNetBridge(url) {
  closeNetBridge();
  if (!url) {
    return false;
  }

  const generation = netBridgeGeneration;
  postSerial(`[NET] Connecting bridge before boot: ${url}\n`);
  postStatus("Connecting network bridge...", { bootInFlight: true });
  try {
    await connectNetBridge(url, generation);
    postSerial("[NET] Bridge ready before boot; NE2000 DHCP will use the helper.\n");
    return true;
  } catch (error) {
    if (generation === netBridgeGeneration) {
      postSerial(`[NET] ${error instanceof Error ? error.message : String(error)}; using internal gateway.\n`);
      postStatus("Network bridge unavailable; NE2000 internal gateway is active.", { bootInFlight: true });
    }
    return false;
  }
}

function destroyMachine() {
  closeNetBridge();
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
    MAX_FRAME_BYTES,
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
  if (currentSpeed === 0) {
    scheduleFastTick();
  } else {
    setTimeout(runTick, 0);
  }
}

function stepFullSpeedSlice(module) {
  const deadline = performance.now() + FULL_SPEED_SLICE_MS;
  let rc = 1;

  do {
    rc = module._be300_step(machineHandle, batchesPerTick);
    drainNetTx(module);
  } while (rc > 0 && running && performance.now() < deadline);

  return rc;
}

function runTick() {
  tickScheduled = false;
  if (!running || !moduleInstance || !machineHandle) {
    return;
  }

  const rc = currentSpeed === 0
    ? stepFullSpeedSlice(moduleInstance)
    : moduleInstance._be300_step(machineHandle, batchesPerTick);
  drainSerial(moduleInstance);
  drainFrame(moduleInstance);
  drainNetTx(moduleInstance);

  if (rc > 0) {
    scheduleTick();
    return;
  }

  running = false;
  postStatus("Emulator stopped.");
}

function stageBytes(module, bytes, loadFn, failMessage) {
  const ptr = allocBytes(module, bytes);
  const rc = loadFn(ptr, bytes.length);
  module._free(ptr);
  if (rc !== 0) {
    throw new Error(failMessage);
  }
}

async function bootNand(nandBytes, bootConfig, cfSlot0Bytes = null, cfSlot1Bytes = null) {
  const module = await ensureModule();
  const config = normalizeBootConfig(bootConfig);
  let flags = 0;
  let macPtr = 0;

  destroyMachine();
  ensureScratch(module);
  ensureNetScratch(module);

  // Stage the NAND image bytes into MEMFS at the path our glue uses for
  // cfg.nand_path, then ask the existing public be300_create() to mount it.
  stageBytes(
    module,
    nandBytes,
    (ptr, len) => module._be300_web_load_nand(ptr, len),
    "Failed to stage NAND image into MEMFS.",
  );

  if (cfSlot0Bytes) {
    stageBytes(
      module,
      cfSlot0Bytes,
      (ptr, len) => module._be300_web_load_cf(0, ptr, len),
      "Failed to stage primary CF image into MEMFS.",
    );
    flags |= WEB_FLAG_CF0;
  }
  if (cfSlot1Bytes) {
    stageBytes(
      module,
      cfSlot1Bytes,
      (ptr, len) => module._be300_web_load_cf(1, ptr, len),
      "Failed to stage secondary CF image into MEMFS.",
    );
    flags |= WEB_FLAG_CF1;
  }

  if (config.enableNe2000) {
    flags |= WEB_FLAG_NE2000;
  }
  if (config.enableTargusKeyboard) {
    flags |= WEB_FLAG_TARGUS_KEYBOARD;
  }
  if (config.netMac) {
    flags |= WEB_FLAG_NET_MAC;
    macPtr = allocBytes(module, config.netMac);
  }

  {
    const fbWidth = config.fbSize === "480x640" ? 480 : 240;
    const fbHeight = config.fbSize === "480x640" ? 640 : 320;
    machineHandle = module._be300_web_create_ex2(
      config.sdramMb,
      config.targetMhz,
      flags,
      macPtr,
      fbWidth,
      fbHeight,
    );
  }
  if (macPtr) {
    module._free(macPtr);
  }
  if (!machineHandle) {
    postMessage({ type: "fatal", message: "Failed to create BE-300 machine." });
    return;
  }

  const bridgeReady = config.enableNe2000 && config.netBridgeUrl
    ? await prepareNetBridge(config.netBridgeUrl)
    : false;

  if (config.enableNe2000 && bridgeReady) {
    postSerial("[NET] NE2000 using local network bridge.\n");
  } else if (config.enableNe2000) {
    postSerial("[NET] NE2000 using GXemul internal IPv4 gateway.\n");
  }

  cachedNand = nandBytes;
  cachedCfSlot0 = cfSlot0Bytes;
  cachedCfSlot1 = cfSlot1Bytes;
  cachedBootConfig = config;
  currentSpeed = config.targetMhz;
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

function loadOptionalBytes(buffer) {
  return buffer ? new Uint8Array(buffer) : null;
}

self.addEventListener("message", async ({ data }) => {
  switch (data.type) {
    case "bootNand": {
      try {
        postStatus("Loading NAND image...", { bootInFlight: true });
        const nandBytes = await loadNandBytes(data);
        const cfSlot0Bytes = loadOptionalBytes(data.cfSlot0Bytes);
        const cfSlot1Bytes = loadOptionalBytes(data.cfSlot1Bytes);
        await bootNand(nandBytes, data, cfSlot0Bytes, cfSlot1Bytes);
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
        try {
          postStatus("Resetting emulator...", { bootInFlight: true });
          await bootNand(cachedNand, cachedBootConfig, cachedCfSlot0, cachedCfSlot1);
        } catch (error) {
          destroyMachine();
          postMessage({
            type: "fatal",
            message: `Reset failed: ${error instanceof Error ? error.message : String(error)}`,
          });
        }
      }
      break;
    case "stop":
      if (moduleInstance && machineHandle) {
        moduleInstance._be300_stop(machineHandle);
        closeNetBridge();
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
    case "setSpeed": {
      const speed = normalizeSpeed(data.targetMhz);
      currentSpeed = speed;
      batchesPerTick = getBatchesPerTick(speed);
      if (cachedBootConfig) {
        cachedBootConfig.targetMhz = speed;
      }
      break;
    }
    case "stowawayKey":
      if (moduleInstance && machineHandle) {
        moduleInstance._be300_web_stowaway_key(data.scancode, data.release ? 1 : 0);
      }
      break;
    default:
      break;
  }
});

import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import vm from "node:vm";
import * as config from "../web/launch-config.mjs";
import * as buttonRegions from "../web/button-regions.mjs";
import * as audio from "../web/audio.mjs";

const imageIds = ["300", "beshell", "expod", "net", "glaze", "mw-2.4", "opie-4.2"];
const parse = (search) => config.parseLaunchConfig(search, imageIds);

test("plain visits and unrelated query parameters preserve defaults", () => {
  assert.deepEqual(parse(""), { settings: {}, boot: false, errors: [] });
  assert.deepEqual(parse("?utm_source=glaze"), parse(""));
});

test("hosted images and boot booleans", () => {
  for (const image of imageIds) {
    assert.equal(parse(`?image=${image}&boot=1`).settings.image, image);
    assert.equal(parse(`?image=${image}&boot=true`).boot, true);
  }
  for (const boot of ["0", "false"]) assert.equal(parse(`?boot=${boot}`).boot, false);
  assert.equal(parse("?image=glaze").boot, false);
});

test("all URL settings, explicit false, empty bridge and decoded values", () => {
  const result = parse("?image=glaze&boot=1&card=cfblank&cfSize=128&keyboard=0&audio=false&speed=64&mac=10%3A20%3A30%3A00%3A00%3A10&bridge=");
  assert.deepEqual(result, {
    settings: { image: "glaze", card: "cfblank", cfSize: "128", speed: "64", keyboard: false, audio: false, mac: "10:20:30:00:00:10", bridge: "" },
    boot: true, errors: [],
  });
  assert.equal(parse("?keyboard=true&audio=1&speed=0").settings.keyboard, true);
  assert.equal(parse("?bridge=wss%3A%2F%2Fexample.com%2Fnet%3Fa%3D1%26b%3D2").errors.length, 0);
});

test("invalid or duplicate recognized parameters prevent automatic boot", () => {
  for (const query of [
    "image=upload", "image=https://example.com/x.bin", "image=../nand.bin", "image=missing",
    "card=cf0", "card=bogus", "cfSize=17", "speed=-1", "speed=65", "speed=1.5", "speed=2junk", "speed=Infinity", "speed=",
    "keyboard=on", "audio=off", "boot=maybe", "mac=ff:00:00:00:00:00", "mac=bad",
    "bridge=https://example.com", "bridge=javascript:alert(1)", "bridge=ws://",
    "image=glaze&image=300", "audio=1&audio=0", "boot=1&boot=0",
  ]) {
    const result = parse(`?boot=1&${query}`);
    assert.equal(result.boot, false, query);
    assert.ok(result.errors.length > 0, query);
  }
});

test("network validation preserves manual UI behavior", () => {
  assert.deepEqual(config.parseMacAddress(""), { valid: true, bytes: null });
  assert.deepEqual([...config.parseMacAddress("10:20:30:00:00:10").bytes], [16, 32, 48, 0, 0, 16]);
  assert.equal(config.parseMacAddress("11:20:30:00:00:10").valid, false);
  assert.deepEqual(config.normalizeBridgeUrl(" "), { valid: true, url: "" });
  assert.deepEqual(config.normalizeBridgeUrl(" ws://localhost:8765 "), { valid: true, url: "ws://localhost:8765/" });
});

// Run the actual frontend initialization and Boot handler with a minimal DOM
// and a recording Worker. This checks the configuration sent to the worker,
// not a second implementation of the URL-to-boot flow. No browser is needed.
async function frontend(search) {
  class Target {
    listeners = new Map();
    dataset = {};
    classList = { add() {}, remove() {}, toggle() {} };
    captures = new Set();
    value = "";
    tagName = "DIV";
    checked = false;
    files = [];
    textContent = "";
    addEventListener(type, fn) {
      const list = this.listeners.get(type) ?? [];
      list.push(fn); this.listeners.set(type, list);
    }
    async fire(type, event = {}) {
      event = { type, target: this, preventDefault() {}, ...event };
      for (const fn of this.listeners.get(type) ?? []) await fn(event);
    }
    replaceChildren() {}
    hasPointerCapture(id) { return this.captures.has(id); }
    setPointerCapture(id) { this.captures.add(id); }
    releasePointerCapture(id) { this.captures.delete(id); }
    setAttribute() {}
    getBoundingClientRect() { return { left: 0, top: 0, width: 1024, height: 1600 }; }
  }
  const elements = new Map();
  const element = (id) => {
    if (!elements.has(id)) elements.set(id, new Target());
    return elements.get(id);
  };
  for (const [id, value] of Object.entries({ nandSelect: "300", primarySocket: "ne2000", cfBlankSize: "64", speedControl: "0", netBridgeUrl: "ws://127.0.0.1:8765" })) element(id).value = value;
  element("targusKeyboardEnabled").checked = true;
  element("audioEnabled").checked = true;
  element("screen").width = 240; element("screen").height = 320;
  element("screen").getContext = () => ({ createImageData: () => ({}) });
  const messages = [];
  let worker;
  class Worker extends Target {
    constructor() { super(); worker = this; }
    postMessage(message) { messages.push(message); }
  }
  const window = new Target(); window.location = { search };
  let audioContexts = 0;
  const audioBuffers = [], audioStarts = [];
  window.AudioContext = class {
    state = "suspended";
    currentTime = 1;
    createBuffer(channels, frames, rate) {
      const data = Array.from({ length: channels }, () => new Float32Array(frames));
      const buffer = { numberOfChannels: channels, length: frames, sampleRate: rate,
        duration: frames / rate, getChannelData: (channel) => data[channel] };
      audioBuffers.push(buffer); return buffer;
    }
    createBufferSource() {
      return { connect() {}, start(at) { audioStarts.push({ buffer: this.buffer, at }); } };
    }
    constructor() { audioContexts++; }
    async resume() { this.state = "running"; }
  };
  const document = new Target();
  document.body = new Target();
  document.querySelector = (selector) => element(selector.replace(/^#/, ""));
  let maskImage;
  class Image extends Target {
    naturalWidth = 1024;
    naturalHeight = 1536;
    constructor() { super(); maskImage = this; }
  }
  // Supply opaque mask pixels at the tested button locations. Hit testing and
  // pointer ownership still run through the actual frontend handlers.
  const maskPixels = new Uint8ClampedArray(1024 * 1536 * 4);
  for (const [x, y] of [[512, 1175], [512, 1259], [843, 1206], [180, 1206]]) {
    maskPixels.fill(255, (y * 1024 + x) * 4, (y * 1024 + x) * 4 + 4);
  }
  document.createElement = () => ({ getContext: () => ({
    drawImage() {}, getImageData: () => ({ data: maskPixels }),
  }) });
  const rocket = element("rocket");
  rocket.dataset = { set: "btn2", mask: "16", toggle: "true" };
  document.querySelectorAll = () => [rocket];
  let now = 0, nextTimer = 1;
  const timers = new Map();
  const fakeTimeout = (fn, delay) => {
    const id = nextTimer++;
    timers.set(id, { fn, due: now + delay });
    return id;
  };
  const advance = (ms) => {
    now += ms;
    for (const [id, timer] of timers) {
      if (timer.due <= now) { timers.delete(id); timer.fn(); }
    }
  };
  const context = vm.createContext({ window, document, Worker, Image, Element: Target, URL, URLSearchParams, console,
    performance: { now: () => now }, setTimeout: fakeTimeout, clearTimeout: (id) => timers.delete(id) });
  const synthetic = (namespace) => {
    const names = Object.keys(namespace);
    return new vm.SyntheticModule(names, function () {
      for (const name of names) this.setExport(name, namespace[name]);
    }, { context });
  };
  const dependencies = {
    "./launch-config.mjs": synthetic(config),
    "./button-regions.mjs": synthetic(buttonRegions),
    "./audio.mjs": synthetic(audio),
  };
  const module = new vm.SourceTextModule(await readFile(new URL("../web/app.js", import.meta.url), "utf8"), {
    context, initializeImportMeta(meta) { meta.url = "https://linux4be.com/app.js"; },
  });
  await module.link((specifier) => dependencies[specifier.split("?")[0]]);
  await module.evaluate();
  await maskImage.fire("load");
  return { element, messages, worker, window, document, advance, audioBuffers, audioStarts, audioContexts: () => audioContexts };
}

test("Glaze link boots exactly once with its selected configuration", async () => {
  const app = await frontend("?image=glaze&boot=1&card=none&keyboard=0");
  const boots = app.messages.filter((m) => m.type === "bootNand");
  assert.equal(boots.length, 1);
  assert.equal(boots[0].nandName, "glaze.bin");
  assert.match(boots[0].nandUrl, /^\.\/nand\/glaze\.bin\?v=/);
  assert.equal(boots[0].enableNe2000, false);
  assert.equal(boots[0].enableTargusKeyboard, false);
  assert.equal(boots[0].enableAudio, true);
  assert.equal(boots[0].sdramMb, 16);
  assert.equal(boots[0].targetMhz, 0);
  assert.equal(app.element("bootBtn").disabled, true);
  assert.equal(app.audioContexts(), 0, "autoboot does not create a suspended audio context");
  await app.element("bootBtn").fire("click");
  assert.equal(app.messages.filter((m) => m.type === "bootNand").length, 1, "no duplicate boot while loading");
  await app.window.fire("pointerdown");
  assert.equal(app.audioContexts(), 1, "sound starts on interaction");
});

test("configuration-only links wait for Boot; plain visits retain defaults", async () => {
  const app = await frontend("?image=glaze&card=none&audio=0");
  assert.equal(app.messages.filter((m) => m.type === "bootNand").length, 0);
  assert.equal(app.element("nandSelect").value, "glaze");
  await app.element("bootBtn").fire("click");
  assert.equal(app.messages.find((m) => m.type === "bootNand").nandName, "glaze.bin");
  assert.equal(app.audioContexts(), 0);
  const plain = await frontend("");
  await plain.element("bootBtn").fire("click");
  const boot = plain.messages.find((m) => m.type === "bootNand");
  assert.equal(boot.nandName, "300.bin");
  assert.equal(boot.enableNe2000, true);
  assert.equal(boot.enableTargusKeyboard, true);
  assert.equal(plain.audioContexts(), 1);
});

test("blank CF and network URL settings reach the worker", async () => {
  const cf = await frontend("?image=glaze&card=cfblank&cfSize=32&speed=12&audio=0&boot=1");
  const cfBoot = cf.messages.find((m) => m.type === "bootNand");
  assert.equal(cfBoot.cfSlot0Blank.sizeMb, 32);
  assert.equal(cfBoot.cfSlot0Blank.files.length, 0);
  assert.equal(cfBoot.targetMhz, 12);
  assert.equal(cfBoot.enableNe2000, false);
  assert.equal(cf.element("cfBlankPanel").hidden, false);
  const net = await frontend("?card=ne2000&bridge=&mac=10:20:30:00:00:10&boot=1");
  const netBoot = net.messages.find((m) => m.type === "bootNand");
  assert.equal(netBoot.netBridgeUrl, "");
  assert.deepEqual([...netBoot.netMac], [16, 32, 48, 0, 0, 16]);
  assert.equal(netBoot.enableNe2000, true);
});

test("invalid URLs show an error and never silently boot the default image", async () => {
  const app = await frontend("?image=typo&boot=1");
  assert.equal(app.messages.filter((m) => m.type === "bootNand").length, 0);
  assert.equal(app.element("status").dataset.state, "error");
  assert.match(app.element("status").textContent, /Invalid URL parameter "image"/);
});


test("frontend schedules stereo frames with the correct duration and honors mute", async () => {
  const app = await frontend("?image=glaze");
  await app.element("bootBtn").fire("click");
  const samples = new Int16Array(441 * 2);
  for (let i = 0; i < 441; i++) samples[i * 2] = 16384;
  const packet = { type: "audio", frames: 441, channels: 2, sampleRate: 44100, samples };
  await app.worker.fire("message", { data: packet });
  await app.worker.fire("message", { data: packet });
  assert.equal(app.audioBuffers.length, 2);
  const buffer = app.audioBuffers[0];
  assert.equal(buffer.numberOfChannels, 2);
  assert.equal(buffer.length, 441);
  assert.equal(buffer.duration, 0.01);
  assert.ok([...buffer.getChannelData(0)].every((v) => v === 0.5));
  assert.ok([...buffer.getChannelData(1)].every((v) => v === 0));
  assert.ok(Math.abs(app.audioStarts[1].at - app.audioStarts[0].at - 0.01) < 1e-10);
  app.element("audioEnabled").checked = false;
  await app.element("audioEnabled").fire("change");
  await app.worker.fire("message", { data: packet });
  assert.equal(app.audioStarts.length, 2);
  assert.equal(app.messages.at(-1).type, "setAudio");
  assert.equal(app.messages.at(-1).enabled, false);
});

async function runningFrontend(keyboard = false) {
  const app = await frontend(`?boot=1&keyboard=${keyboard ? 1 : 0}&audio=0`);
  await app.worker.fire("message", { data: { type: "status", running: true, bootInFlight: false } });
  return app;
}
const lastButtons = (app) => app.messages.filter((m) => m.type === "setButtons").at(-1);
const framePress = (app, pointerId, x, y) => app.element(".device-frame").fire("pointerdown", {
  pointerId, clientX: x, clientY: y * 1600 / 1536,
});

test("keyboard, keypad and pointer independently hold the same device button", async () => {
  const app = await runningFrontend();
  await app.window.fire("keydown", { code: "Enter" });
  await app.window.fire("keydown", { code: "NumpadEnter" });
  await framePress(app, 1, 512, 1259);
  assert.equal(lastButtons(app).btnSet1, 4);
  app.advance(130);
  await app.window.fire("keyup", { code: "Enter" });
  assert.equal(lastButtons(app).btnSet1, 4);
  await app.element(".device-frame").fire("pointerup", { pointerId: 1 });
  assert.equal(lastButtons(app).btnSet1, 4, "keypad still owns OK");
  await app.window.fire("keyup", { code: "NumpadEnter" });
  assert.equal(lastButtons(app).btnSet1, 0);
});

test("two frame pointers hold Power+Up and release independently", async () => {
  const app = await runningFrontend();
  await framePress(app, 1, 843, 1206);
  await framePress(app, 2, 512, 1175);
  assert.equal(lastButtons(app).btnSet1, 16);
  assert.equal(lastButtons(app).btnSet2, 128);
  app.advance(130);
  await app.element(".device-frame").fire("lostpointercapture", { pointerId: 1 });
  assert.equal(lastButtons(app).btnSet1, 16);
  assert.equal(lastButtons(app).btnSet2, 0);
  await app.element(".device-frame").fire("pointercancel", { pointerId: 2 });
  assert.equal(lastButtons(app).btnSet1, 0);
});

test("minimum dwell survives duplicate releases and a new press cancels old release", async () => {
  const app = await runningFrontend();
  await app.window.fire("keydown", { code: "ArrowUp" });
  await app.window.fire("keyup", { code: "ArrowUp" });
  await app.window.fire("keyup", { code: "ArrowUp" });
  assert.equal(lastButtons(app).btnSet1, 16);
  app.advance(60);
  await app.window.fire("keydown", { code: "ArrowUp" });
  app.advance(70);
  assert.equal(lastButtons(app).btnSet1, 16, "old release cannot clear the new press");
  await app.window.fire("keyup", { code: "ArrowUp" });
  app.advance(50);
  assert.equal(lastButtons(app).btnSet1, 0);
});

test("focus loss and hidden pages release keys, pointers, latches and touch immediately", async () => {
  for (const hidden of [false, true]) {
    const app = await runningFrontend();
    await framePress(app, 1, 843, 1206);
    await app.window.fire("keydown", { code: "ArrowUp" });
    await app.element("rocket").fire("click");
    await app.element("screen").fire("pointerdown", { pointerId: 3, clientX: 40, clientY: 80 });
    if (hidden) {
      app.document.hidden = true;
      await app.document.fire("visibilitychange");
    } else await app.window.fire("blur");
    assert.equal(lastButtons(app).btnSet1, 0);
    assert.equal(lastButtons(app).btnSet2, 0);
    assert.equal(app.messages.filter((m) => m.type === "setTouch").at(-1).down, false);
    assert.equal(app.element(".device-frame").hasPointerCapture(1), false);
    app.advance(1000);
    assert.equal(lastButtons(app).btnSet1, 0, "no delayed callback restores old state");
    await framePress(app, 1, 843, 1206);
    assert.equal(lastButtons(app).btnSet2, 128, "pointer ID is reusable");
  }
});

test("Rocket latch and pointer have independent ownership", async () => {
  const app = await runningFrontend();
  await app.element("rocket").fire("click");
  await framePress(app, 1, 180, 1206);
  await app.element("rocket").fire("click");
  assert.equal(lastButtons(app).btnSet2, 16);
  app.advance(130);
  await app.element(".device-frame").fire("pointerup", { pointerId: 1 });
  assert.equal(lastButtons(app).btnSet2, 0);
});

test("F12 drives Power with either keyboard mode; dock aliases release on the last key", async () => {
  const noDock = await runningFrontend();
  await noDock.window.fire("keydown", { code: "F12" });
  await noDock.window.fire("keydown", { code: "ArrowUp" });
  assert.equal(lastButtons(noDock).btnSet1, 16);
  assert.equal(lastButtons(noDock).btnSet2, 128);
  await noDock.window.fire("blur");
  const app = await runningFrontend(true);
  await app.window.fire("keydown", { code: "F12" });
  assert.equal(lastButtons(app).btnSet2, 128);
  app.advance(130);
  await app.window.fire("keyup", { code: "F12" });
  assert.equal(lastButtons(app).btnSet2, 0);
  await app.window.fire("keydown", { code: "ControlLeft" });
  await app.window.fire("keydown", { code: "ControlRight" });
  await app.window.fire("keyup", { code: "ControlLeft" });
  let dock = app.messages.filter((m) => m.type === "stowawayKey");
  assert.equal(dock.length, 1);
  assert.equal(dock[0].scancode, 26);
  assert.equal(dock[0].release, false);
  await app.window.fire("blur");
  dock = app.messages.filter((m) => m.type === "stowawayKey");
  assert.equal(dock.length, 2);
  assert.equal(dock[1].release, true);
});

test("stop clears pointer ownership and pending releases before a new boot", async () => {
  const app = await runningFrontend();
  await framePress(app, 1, 843, 1206);
  await app.window.fire("keydown", { code: "ArrowUp" });
  await app.window.fire("keyup", { code: "ArrowUp" });
  await app.element("stopBtn").fire("click");
  assert.equal(lastButtons(app).btnSet1, 0);
  assert.equal(lastButtons(app).btnSet2, 0);
  await app.worker.fire("message", { data: { type: "status", running: false, bootInFlight: false } });
  app.advance(1000);
  await app.element(".device-frame").fire("lostpointercapture", { pointerId: 1 });
  await app.worker.fire("message", { data: { type: "status", running: true, bootInFlight: false } });
  await framePress(app, 1, 512, 1175);
  assert.equal(lastButtons(app).btnSet1, 16);
  assert.equal(lastButtons(app).btnSet2, 0);
});

import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import vm from "node:vm";
import * as config from "../web/launch-config.mjs";

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
    classList = { add() {}, remove() {} };
    value = "";
    checked = false;
    files = [];
    textContent = "";
    addEventListener(type, fn) {
      const list = this.listeners.get(type) ?? [];
      list.push(fn); this.listeners.set(type, list);
    }
    async fire(type, event = {}) { for (const fn of this.listeners.get(type) ?? []) await fn(event); }
    replaceChildren() {}
    hasPointerCapture() { return false; }
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
  window.AudioContext = class {
    state = "suspended";
    constructor() { audioContexts++; }
    async resume() { this.state = "running"; }
  };
  const document = new Target();
  document.body = new Target();
  document.querySelector = (selector) => element(selector.replace(/^#/, ""));
  document.querySelectorAll = () => [];
  const context = vm.createContext({ window, document, Worker, Image: Target, URL, URLSearchParams, console, performance, setTimeout, clearTimeout });
  const exports = Object.keys(config);
  const dependency = new vm.SyntheticModule(exports, function () {
    for (const name of exports) this.setExport(name, config[name]);
  }, { context });
  const module = new vm.SourceTextModule(await readFile(new URL("../web/app.js", import.meta.url), "utf8"), {
    context, initializeImportMeta(meta) { meta.url = "https://linux4be.com/app.js"; },
  });
  await module.link(() => dependency);
  await module.evaluate();
  return { element, messages, worker, window, audioContexts: () => audioContexts };
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

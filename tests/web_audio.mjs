import assert from "node:assert/strict";
import test from "node:test";
import { AUDIO_CHANNELS, AUDIO_SAMPLE_RATE, createAudioBuffer, drainAudioPacket } from "../web/audio.mjs";

function context() {
  return { createBuffer(channels, frames, rate) {
    const data = Array.from({ length: channels }, () => new Float32Array(frames));
    return { numberOfChannels: channels, length: frames, sampleRate: rate, duration: frames / rate,
      getChannelData: (channel) => data[channel] };
  } };
}
test("WASM drain capacity and result are stereo frames, with an owned transfer buffer", () => {
  const heap = new Int16Array([91, 92, -32768, 0, 0, 32767, 1234, -1234, 93, 94]);
  const module = { HEAP16: heap, _be300_web_audio_drain(pointer, frames) {
    assert.equal(pointer, 4); assert.equal(frames, 3); return 3;
  } };
  const packet = drainAudioPacket(module, 4, 3);
  assert.deepEqual(packet, { type: "audio", channels: 2, sampleRate: 44100, frames: 3,
    samples: new Int16Array([-32768, 0, 0, 32767, 1234, -1234]) });
  heap.fill(0);
  assert.equal(packet.samples[0], -32768);
  const buffer = createAudioBuffer(context(), packet);
  assert.equal(buffer.numberOfChannels, AUDIO_CHANNELS);
  assert.equal(buffer.length, 3);
  assert.equal(buffer.sampleRate, AUDIO_SAMPLE_RATE);
  assert.equal(buffer.duration, 3 / 44100);
  assert.deepEqual([...buffer.getChannelData(0)], [-1, 0, 1234 / 32768]);
  assert.deepEqual([...buffer.getChannelData(1)], [0, 32767 / 32768, -1234 / 32768]);
});
test("empty drains stop; invalid results cannot overread the WASM heap", () => {
  const module = { HEAP16: new Int16Array(8), _be300_web_audio_drain: () => 0 };
  assert.equal(drainAudioPacket(module, 0), null);
  for (const frames of [-1, 5, 1.5]) {
    module._be300_web_audio_drain = () => frames;
    assert.throws(() => drainAudioPacket(module, 0, 4), RangeError);
  }
  module._be300_web_audio_drain = () => 4;
  assert.throws(() => drainAudioPacket(module, 2, 4), RangeError);
  assert.throws(() => drainAudioPacket(module, 1, 4), RangeError);
});
test("bad, partial, or old mono packets are dropped", () => {
  const base = { channels: 2, sampleRate: 44100, frames: 2, samples: new Int16Array(4) };
  for (const packet of [null, {...base, channels: 1}, {...base, frames: 1.5},
    {...base, frames: 0}, {...base, sampleRate: 22050}, {...base, samples: new Int16Array(3)}]) {
    assert.equal(createAudioBuffer(context(), packet), null);
  }
});

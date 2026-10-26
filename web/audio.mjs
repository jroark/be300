// The shared host sink sends signed S16 stereo at 44.1 kHz. All capacities
// and drain results are frames; one frame contains two interleaved samples.
export const AUDIO_CHANNELS = 2;
export const AUDIO_SAMPLE_RATE = 44100;
export const AUDIO_DRAIN_FRAMES = 4096;

export function drainAudioPacket(module, pointer, maxFrames = AUDIO_DRAIN_FRAMES) {
  const frames = module._be300_web_audio_drain(pointer, maxFrames);
  if (frames === 0) return null;
  const start = pointer >>> 1;
  const end = start + frames * AUDIO_CHANNELS;
  if (!Number.isInteger(frames) || frames < 0 || frames > maxFrames ||
      pointer < 0 || pointer % 2 !== 0 || end > module.HEAP16.length) {
    throw new RangeError("Invalid stereo audio drain result");
  }
  return {
    type: "audio", channels: AUDIO_CHANNELS, sampleRate: AUDIO_SAMPLE_RATE,
    frames, samples: module.HEAP16.slice(start, end),
  };
}

export function createAudioBuffer(context, packet) {
  if (!packet || packet.channels !== AUDIO_CHANNELS ||
      packet.sampleRate !== AUDIO_SAMPLE_RATE || !Number.isInteger(packet.frames) ||
      packet.frames <= 0 || !packet.samples ||
      packet.samples.length !== packet.frames * AUDIO_CHANNELS) return null;
  const buffer = context.createBuffer(AUDIO_CHANNELS, packet.frames, AUDIO_SAMPLE_RATE);
  for (let channel = 0; channel < AUDIO_CHANNELS; channel++) {
    const output = buffer.getChannelData(channel);
    for (let i = 0; i < packet.frames; i++) {
      output[i] = packet.samples[i * AUDIO_CHANNELS + channel] / 32768;
    }
  }
  return buffer;
}

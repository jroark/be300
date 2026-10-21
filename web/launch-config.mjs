// URL configuration is limited to settings the frontend already exposes.
// Image identifiers refer to the hosted catalogue, never arbitrary URLs.
export function parseMacAddress(value) {
  const text = value.trim();
  if (!text) return { valid: true, bytes: null };
  if (!/^[0-9a-fA-F]{2}(:[0-9a-fA-F]{2}){5}$/.test(text)) {
    return { valid: false, bytes: null };
  }
  const bytes = Uint8Array.from(text.split(":"), (part) => Number.parseInt(part, 16));
  return { valid: (bytes[0] & 1) === 0, bytes: (bytes[0] & 1) === 0 ? bytes : null };
}

export function normalizeBridgeUrl(value) {
  const text = value.trim();
  if (!text) return { valid: true, url: "" };
  try {
    const url = new URL(text);
    if (url.protocol === "ws:" || url.protocol === "wss:") {
      return { valid: true, url: url.href };
    }
  } catch { /* Report an invalid setting below. */ }
  return { valid: false, url: "" };
}

export function parseLaunchConfig(search, imageIds) {
  const params = new URLSearchParams(search);
  const settings = {};
  const errors = [];
  let boot = false;
  const valid = {
    image: (v) => imageIds.includes(v),
    card: (v) => ["none", "ne2000", "cfblank"].includes(v),
    cfSize: (v) => ["16", "32", "64", "128", "256", "512"].includes(v),
    speed: (v) => /^(0|[1-9]\d*)$/.test(v) && Number(v) <= 64,
    keyboard: (v) => ["0", "1", "false", "true"].includes(v),
    audio: (v) => ["0", "1", "false", "true"].includes(v),
    boot: (v) => ["0", "1", "false", "true"].includes(v),
    mac: (v) => parseMacAddress(v).valid,
    bridge: (v) => normalizeBridgeUrl(v).valid,
  };
  for (const [name, validate] of Object.entries(valid)) {
    if (!params.has(name)) continue;
    if (params.getAll(name).length !== 1) {
      errors.push(`URL parameter "${name}" must appear only once.`);
      continue;
    }
    const value = params.get(name);
    if (!validate(value)) {
      errors.push(`Invalid URL parameter "${name}". See the launch-link documentation.`);
      continue;
    }
    if (name === "boot") boot = value === "1" || value === "true";
    else if (name === "keyboard" || name === "audio") settings[name] = value === "1" || value === "true";
    else settings[name] = value;
  }
  // A typo must never automatically boot a different, default configuration.
  return { settings, boot: boot && errors.length === 0, errors };
}

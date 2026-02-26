# Patched Unicorn 2.1.4 (macOS 26 workaround)

This directory carries a patched copy of Homebrew's `libunicorn.2.dylib`
(2.1.4).  macOS 26.3 kills any process that executes `MRS x?, CTR_EL0` from
EL0.  Unicorn's `init_cache_info` issues that instruction unconditionally
when the first mapping is created, causing every `uc_mem_map()` call to die
with `Illegal instruction: 4` before the emulator can boot.

Patch summary:

- Offset `0xE550`: replace `d53b0028` (`mrs x8, CTR_EL0`) with
  `52800008` (`mov w8, #0`).  Cacheline size defaults to 4 words, which
   matches the old fallback path in `init_cache_info`.
- Update the install name to `@rpath/libunicorn.2.dylib` so the build can
  link against this local copy instead of the Homebrew keg path.
- Ad-hoc codesign (`codesign --force --sign -`) so macOS will load the
  modified dylib.

If the library ever needs to be re-generated:

```bash
brew reinstall unicorn # optional, to refresh source bits
cp /opt/homebrew/Cellar/unicorn/2.1.4/lib/libunicorn.2.dylib third_party/unicorn/
chmod u+w third_party/unicorn/libunicorn.2.dylib
python3 - <<'PY'
from pathlib import Path
path = Path('third_party/unicorn/libunicorn.2.dylib')
data = bytearray(path.read_bytes())
needle = bytes.fromhex('28003bd5')  # d53b0028 (mrs x8, CTR_EL0)
replace = bytes.fromhex('08008052') # 52800008 (mov w8, #0)
idx = data.find(needle)
assert idx != -1, 'pattern not found'
data[idx:idx+4] = replace
path.write_bytes(data)
print(f'Patched CTR_EL0 probe at offset 0x{idx:x}')
PY
install_name_tool -id @rpath/libunicorn.2.dylib third_party/unicorn/libunicorn.2.dylib
codesign --force --sign - third_party/unicorn/libunicorn.2.dylib
ln -sf libunicorn.2.dylib third_party/unicorn/libunicorn.dylib
```

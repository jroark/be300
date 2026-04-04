import Foundation

final class EmulatorEngine: ObservableObject {
    private var machine: UnsafeMutablePointer<machine_t>?
    private var emulationQueue = DispatchQueue(label: "com.be300.emulation", qos: .userInteractive)
    private var _running = false
    private let lock = NSLock()

    /// RGBA8888 staging buffer (240 * 320 * 4 bytes)
    private(set) var frameBuffer = Data(count: 240 * 320 * 4)
    private(set) var frameWidth: UInt32 = 240
    private(set) var frameHeight: UInt32 = 320

    @Published var isRunning = false
    @Published var serialOutput = ""

    func boot() {
        guard machine == nil else { return }

        machine = be300_create_web(16, 166, false)
        guard let m = machine else {
            print("[EmulatorEngine] Failed to create emulator")
            return
        }

        guard let kernelURL = Bundle.main.url(forResource: "vmlinux-pgui-demo", withExtension: nil) else {
            print("[EmulatorEngine] Kernel not found in bundle")
            return
        }

        do {
            let kernelData = try Data(contentsOf: kernelURL)
            let cmdline = "console=tty0 root=/dev/ram"
            let result = kernelData.withUnsafeBytes { rawBuf -> Int32 in
                let ptr = rawBuf.baseAddress!
                return cmdline.withCString { cmd in
                    be300_boot_linux_from_memory(m, ptr, rawBuf.count, cmd)
                }
            }
            if result != 0 {
                print("[EmulatorEngine] Failed to load kernel: \(result)")
            }
        } catch {
            print("[EmulatorEngine] Failed to read kernel: \(error)")
        }
    }

    func start() {
        guard let m = machine else { return }

        lock.lock()
        guard !_running else { lock.unlock(); return }
        _running = true
        lock.unlock()

        DispatchQueue.main.async { self.isRunning = true }

        emulationQueue.async { [weak self] in
            guard let self else { return }
            while true {
                self.lock.lock()
                let shouldRun = self._running
                self.lock.unlock()
                if !shouldRun { break }

                let result = be300_step(m, 100)
                if result <= 0 { break }

                self.drainSerial()
            }
            DispatchQueue.main.async { self.isRunning = false }
        }
    }

    func stop() {
        lock.lock()
        _running = false
        lock.unlock()
        if let m = machine {
            be300_stop(m)
        }
    }

    func copyFrame() {
        guard let m = machine else { return }
        var w: UInt32 = 0
        var h: UInt32 = 0
        frameBuffer.withUnsafeMutableBytes { buf in
            let ptr = buf.baseAddress!.assumingMemoryBound(to: UInt8.self)
            be300_copy_frame_rgba8888(m, ptr, buf.count, &w, &h)
        }
        frameWidth = w
        frameHeight = h
    }

    func setTouch(down: Bool, x: UInt16, y: UInt16) {
        guard let m = machine else { return }
        be300_set_touch(m, down, x, y)
    }

    func setButtons(set1: UInt8, set2: UInt8) {
        guard let m = machine else { return }
        be300_set_buttons(m, set1, set2)
    }

    private func drainSerial() {
        guard let m = machine else { return }
        var buf = [CChar](repeating: 0, count: 4096)
        let n = be300_drain_serial(m, &buf, buf.count)
        if n > 0, let str = String(bytes: buf.prefix(n).map { UInt8(bitPattern: $0) }, encoding: .utf8) {
            DispatchQueue.main.async {
                self.serialOutput.append(str)
                // Keep serial output from growing unbounded
                if self.serialOutput.count > 64000 {
                    self.serialOutput = String(self.serialOutput.suffix(32000))
                }
            }
        }
    }

    deinit {
        stop()
        if let m = machine {
            be300_destroy(m)
        }
    }
}

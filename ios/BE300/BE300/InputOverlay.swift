import SwiftUI

struct InputOverlay: View {
    let engine: EmulatorEngine

    // Button bitmasks matching be300_devices.c
    private enum Btn1: UInt8 {
        case ok    = 0x04
        case esc   = 0x08
        case up    = 0x10
        case down  = 0x20
        case right = 0x40
        case left  = 0x80
    }

    private enum Btn2: UInt8 {
        case rocket = 0x10
    }

    @State private var activeSet1: UInt8 = 0
    @State private var activeSet2: UInt8 = 0

    var body: some View {
        HStack(spacing: 40) {
            // D-pad
            dpad

            // Action buttons
            VStack(spacing: 12) {
                actionButton("OK", mask1: .ok)
                actionButton("ESC", mask1: .esc)
                actionButton("Rocket", mask2: .rocket)
            }
        }
        .padding(.vertical, 20)
    }

    private var dpad: some View {
        VStack(spacing: 0) {
            dpadButton("chevron.up", mask: .up)
            HStack(spacing: 24) {
                dpadButton("chevron.left", mask: .left)
                dpadButton("chevron.right", mask: .right)
            }
            dpadButton("chevron.down", mask: .down)
        }
    }

    private func dpadButton(_ icon: String, mask: Btn1) -> some View {
        Image(systemName: icon)
            .font(.title2)
            .frame(width: 56, height: 56)
            .background(activeSet1 & mask.rawValue != 0 ? Color.blue.opacity(0.5) : Color.gray.opacity(0.3))
            .clipShape(Circle())
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { _ in press(mask) }
                    .onEnded { _ in release(mask) }
            )
    }

    private func actionButton(_ label: String, mask1: Btn1? = nil, mask2: Btn2? = nil) -> some View {
        Text(label)
            .font(.caption.bold())
            .frame(width: 64, height: 44)
            .background({
                let active: Bool
                if let m = mask1 { active = activeSet1 & m.rawValue != 0 }
                else if let m = mask2 { active = activeSet2 & m.rawValue != 0 }
                else { active = false }
                return active ? Color.blue.opacity(0.5) : Color.gray.opacity(0.3)
            }())
            .clipShape(RoundedRectangle(cornerRadius: 10))
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { _ in
                        if let m = mask1 { press(m) }
                        else if let m = mask2 { pressSet2(m) }
                    }
                    .onEnded { _ in
                        if let m = mask1 { release(m) }
                        else if let m = mask2 { releaseSet2(m) }
                    }
            )
    }

    private func press(_ mask: Btn1) {
        activeSet1 |= mask.rawValue
        engine.setButtons(set1: activeSet1, set2: activeSet2)
    }

    private func release(_ mask: Btn1) {
        activeSet1 &= ~mask.rawValue
        engine.setButtons(set1: activeSet1, set2: activeSet2)
    }

    private func pressSet2(_ mask: Btn2) {
        activeSet2 |= mask.rawValue
        engine.setButtons(set1: activeSet1, set2: activeSet2)
    }

    private func releaseSet2(_ mask: Btn2) {
        activeSet2 &= ~mask.rawValue
        engine.setButtons(set1: activeSet1, set2: activeSet2)
    }
}

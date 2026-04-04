import SwiftUI

struct EmulatorView: View {
    @ObservedObject var engine: EmulatorEngine
    @State private var showSerial = false

    var body: some View {
        GeometryReader { geo in
            VStack(spacing: 0) {
                // Emulator display with touch overlay
                ZStack {
                    MetalDisplayView(engine: engine)
                        .aspectRatio(3.0 / 4.0, contentMode: .fit)

                    // Touch overlay
                    GeometryReader { displayGeo in
                        Color.clear
                            .contentShape(Rectangle())
                            .gesture(
                                DragGesture(minimumDistance: 0)
                                    .onChanged { value in
                                        let x = UInt16(max(0, min(239, value.location.x / displayGeo.size.width * 240)))
                                        let y = UInt16(max(0, min(319, value.location.y / displayGeo.size.height * 320)))
                                        engine.setTouch(down: true, x: x, y: y)
                                    }
                                    .onEnded { _ in
                                        engine.setTouch(down: false, x: 0, y: 0)
                                    }
                            )
                    }
                }
                .frame(maxHeight: geo.size.height * 0.65)
                .background(Color.black)

                // Controls
                InputOverlay(engine: engine)
                    .frame(maxWidth: .infinity)

                Spacer(minLength: 0)

                // Serial toggle
                if showSerial {
                    ScrollView {
                        Text(engine.serialOutput)
                            .font(.system(size: 10, design: .monospaced))
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .padding(4)
                    }
                    .frame(height: 120)
                    .background(Color.black.opacity(0.9))
                    .foregroundColor(.green)
                }
            }
        }
        .background(Color(uiColor: .systemGroupedBackground))
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                Button(showSerial ? "Hide Serial" : "Serial") {
                    showSerial.toggle()
                }
                .font(.caption)
            }
        }
        .navigationTitle("BE-300")
        .navigationBarTitleDisplayMode(.inline)
    }
}

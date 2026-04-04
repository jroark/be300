import SwiftUI
import MetalKit

struct MetalDisplayView: UIViewRepresentable {
    let engine: EmulatorEngine

    func makeUIView(context: Context) -> MTKView {
        let mtkView = MTKView()
        mtkView.device = MTLCreateSystemDefaultDevice()
        mtkView.preferredFramesPerSecond = 30
        mtkView.enableSetNeedsDisplay = false
        mtkView.isPaused = false
        mtkView.colorPixelFormat = .bgra8Unorm
        mtkView.delegate = context.coordinator
        return mtkView
    }

    func updateUIView(_ uiView: MTKView, context: Context) {}

    func makeCoordinator() -> Renderer {
        Renderer(engine: engine)
    }

    final class Renderer: NSObject, MTKViewDelegate {
        let engine: EmulatorEngine
        private var device: MTLDevice?
        private var commandQueue: MTLCommandQueue?
        private var pipelineState: MTLRenderPipelineState?
        private var texture: MTLTexture?
        private var vertexBuffer: MTLBuffer?

        init(engine: EmulatorEngine) {
            self.engine = engine
            super.init()
        }

        private func setup(device: MTLDevice) {
            self.device = device
            commandQueue = device.makeCommandQueue()

            // Create 240x320 RGBA texture
            let desc = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: .rgba8Unorm,
                width: 240,
                height: 320,
                mipmapped: false
            )
            desc.usage = [.shaderRead]
            texture = device.makeTexture(descriptor: desc)

            // Fullscreen quad vertices (position xy + texcoord uv)
            let vertices: [Float] = [
                -1,  1, 0, 0,  // top-left
                 1,  1, 1, 0,  // top-right
                -1, -1, 0, 1,  // bottom-left
                 1, -1, 1, 1,  // bottom-right
            ]
            vertexBuffer = device.makeBuffer(bytes: vertices, length: vertices.count * MemoryLayout<Float>.size)

            // Shader source
            let shaderSrc = """
            #include <metal_stdlib>
            using namespace metal;
            struct VertexOut {
                float4 position [[position]];
                float2 texcoord;
            };
            vertex VertexOut vertexShader(uint vid [[vertex_id]],
                                          constant float4 *verts [[buffer(0)]]) {
                VertexOut out;
                out.position = float4(verts[vid].xy, 0, 1);
                out.texcoord = verts[vid].zw;
                return out;
            }
            fragment float4 fragmentShader(VertexOut in [[stage_in]],
                                           texture2d<float> tex [[texture(0)]]) {
                constexpr sampler s(mag_filter::nearest, min_filter::nearest);
                return tex.sample(s, in.texcoord);
            }
            """

            do {
                let library = try device.makeLibrary(source: shaderSrc, options: nil)
                let pipelineDesc = MTLRenderPipelineDescriptor()
                pipelineDesc.vertexFunction = library.makeFunction(name: "vertexShader")
                pipelineDesc.fragmentFunction = library.makeFunction(name: "fragmentShader")
                pipelineDesc.colorAttachments[0].pixelFormat = .bgra8Unorm
                pipelineState = try device.makeRenderPipelineState(descriptor: pipelineDesc)
            } catch {
                print("[MetalRenderer] Pipeline setup failed: \(error)")
            }
        }

        func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

        func draw(in view: MTKView) {
            guard let device = view.device else { return }
            if self.device == nil { setup(device: device) }
            guard let pipeline = pipelineState,
                  let texture = texture,
                  let vertexBuffer = vertexBuffer,
                  let commandQueue = commandQueue,
                  let drawable = view.currentDrawable,
                  let passDesc = view.currentRenderPassDescriptor else { return }

            // Copy emulator framebuffer to texture
            engine.copyFrame()
            engine.frameBuffer.withUnsafeBytes { buf in
                let region = MTLRegionMake2D(0, 0, Int(engine.frameWidth), Int(engine.frameHeight))
                texture.replace(region: region, mipmapLevel: 0,
                                withBytes: buf.baseAddress!,
                                bytesPerRow: Int(engine.frameWidth) * 4)
            }

            let cmdBuf = commandQueue.makeCommandBuffer()!
            let encoder = cmdBuf.makeRenderCommandEncoder(descriptor: passDesc)!
            encoder.setRenderPipelineState(pipeline)
            encoder.setVertexBuffer(vertexBuffer, offset: 0, index: 0)
            encoder.setFragmentTexture(texture, index: 0)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
            cmdBuf.present(drawable)
            cmdBuf.commit()
        }
    }
}

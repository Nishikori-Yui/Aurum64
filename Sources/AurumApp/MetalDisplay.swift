// SPDX-License-Identifier: GPL-2.0-or-later
import MetalKit
import SwiftUI

@MainActor
final class GameView: MTKView {
    let input = InputController()
    override var acceptsFirstResponder: Bool { true }
    override func keyDown(with event: NSEvent) { input.key(event.keyCode, pressed: true) }
    override func keyUp(with event: NSEvent) { input.key(event.keyCode, pressed: false) }
    override func resignFirstResponder() -> Bool { input.clear(); return super.resignFirstResponder() }
    override func mouseDown(with event: NSEvent) { window?.makeFirstResponder(self) }
    override func viewDidMoveToWindow() { super.viewDidMoveToWindow(); window?.makeFirstResponder(self) }
}

@MainActor
struct MetalDisplay: NSViewRepresentable {
    var linearFiltering: Bool
    func makeCoordinator() -> DisplayCoordinator { DisplayCoordinator() }
    func makeNSView(context: Context) -> GameView {
        let view = GameView(frame: .zero, device: MTLCreateSystemDefaultDevice())
        view.colorPixelFormat = .bgra8Unorm
        view.clearColor = MTLClearColorMake(0, 0, 0, 1)
        view.preferredFramesPerSecond = 60
        view.delegate = context.coordinator
        view.setAccessibilityLabel("N64 video output")
        context.coordinator.prepare(view)
        return view
    }
    func updateNSView(_ view: GameView, context: Context) { context.coordinator.linear = linearFiltering }
}

@MainActor
final class DisplayCoordinator: NSObject, MTKViewDelegate {
    var linear = false
    private var pipeline: (any MTLRenderPipelineState)?
    private var queue: (any MTLCommandQueue)?
    private var texture: (any MTLTexture)?
    private var pixels = [UInt32](repeating: 0, count: 640 * 576)
    private var lastFrame: UInt64 = .max
    func prepare(_ view: MTKView) {
        guard let device = view.device,
              let url = Bundle.main.url(forResource: "AurumRDP", withExtension: "metallib") else {
            aur_fail("The Metal device or bundled shader library is unavailable."); return
        }
        do {
            let library = try device.makeLibrary(URL: url)
            let descriptor = MTLRenderPipelineDescriptor()
            descriptor.vertexFunction = library.makeFunction(name: "presentVertex")
            descriptor.fragmentFunction = library.makeFunction(name: "presentFragment")
            descriptor.colorAttachments[0].pixelFormat = view.colorPixelFormat
            pipeline = try device.makeRenderPipelineState(descriptor: descriptor)
            queue = device.makeCommandQueue()
        } catch { aur_fail("Cannot create the Metal presentation pipeline: \(error.localizedDescription)") }
    }
    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
    func draw(in view: MTKView) {
        if let game = view as? GameView {
            if view.window?.isKeyWindow == true && view.window?.firstResponder === view { game.input.poll() }
            else { game.input.clear() }
        }
        guard let device = view.device, let pipeline, let queue,
              let drawable = view.currentDrawable, let pass = view.currentRenderPassDescriptor,
              let command = queue.makeCommandBuffer() else { return }
        var snapshot = AurSnapshot(); aur_snapshot(&snapshot)
        if lastFrame != snapshot.frames {
            var width: UInt32 = 0, height: UInt32 = 0
            let count = aur_copy_frame(&pixels, pixels.count, &width, &height)
            if count > 0 && count <= pixels.count && width > 0 && height > 0 {
                if texture?.width != Int(width) || texture?.height != Int(height) {
                    let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: Int(width), height: Int(height), mipmapped: false)
                    d.storageMode = .shared; d.usage = .shaderRead
                    texture = device.makeTexture(descriptor: d)
                }
                pixels.withUnsafeBytes { bytes in
                    if let base = bytes.baseAddress {
                        texture?.replace(region: MTLRegionMake2D(0, 0, Int(width), Int(height)), mipmapLevel: 0, withBytes: base, bytesPerRow: Int(width) * 4)
                    }
                }
            } else { texture = nil }
            lastFrame = snapshot.frames
        }
        guard let encoder = command.makeRenderCommandEncoder(descriptor: pass) else { return }
        if let texture {
            let target = view.drawableSize
            let ratio = 4.0 / 3.0
            let w = min(target.width, target.height * ratio), h = min(target.height, target.width / ratio)
            encoder.setViewport(MTLViewport(originX: (target.width-w)/2, originY: (target.height-h)/2, width: w, height: h, znear: 0, zfar: 1))
            encoder.setRenderPipelineState(pipeline)
            encoder.setFragmentTexture(texture, index: 0)
            encoder.setFragmentBytes(&linear, length: MemoryLayout<Bool>.size, index: 0)
            encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        }
        encoder.endEncoding(); command.present(drawable); command.commit()
        // Presentation uses one shared texture. Complete the read before a subsequent CPU upload.
        command.waitUntilCompleted()
        if command.status == .error {
            aur_fail(command.error?.localizedDescription ?? "Metal presentation failed.")
        }
    }
}

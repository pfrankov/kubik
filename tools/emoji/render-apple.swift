// Render private glyphs using this Mac's Apple Color Emoji. No font is copied.
import Foundation
import CoreGraphics
import CoreText
import ImageIO

let edge = 32
let font = CTFontCreateWithName("AppleColorEmoji" as CFString, 48, nil)

struct Rendered: Codable {
    let sequence: String
    let bitmap: String
    let rgba: String?
}
struct Unsupported: Codable {
    let sequence: String
    let reason: String
}
struct Result: Codable {
    let postscript: String
    let version: String
    let fontURL: String
    let edge: Int
    let glyphs: [Rendered]
    let unsupported: [Unsupported]
}

enum Failure: Error { case invalidArguments, invalidFont, invalidInput, renderFailed }

func makeLine(_ text: String) -> CTLine {
    let key = NSAttributedString.Key(kCTFontAttributeName as String)
    return CTLineCreateWithAttributedString(NSAttributedString(string: text, attributes: [key: font]))
}

func unsupportedReason(_ text: String, _ line: CTLine) -> String? {
    if text.count != 1 { return "not_one_grapheme" }
    let advance = CTLineGetTypographicBounds(line, nil, nil, nil)
    // Apple composes some couples as two overlapping layers (0 + one em).
    // A missing ZWJ ligature instead advances through separate emoji cells.
    if advance <= 0 || advance > Double(CTFontGetSize(font) * 1.05) { return "not_one_composed_cell" }
    for run in CTLineGetGlyphRuns(line) as! [CTRun] {
        let attributes = CTRunGetAttributes(run) as NSDictionary
        guard let rawFont = attributes[kCTFontAttributeName],
              CFGetTypeID(rawFont as CFTypeRef) == CTFontGetTypeID() else { return "missing_run_font" }
        let runFont = rawFont as! CTFont
        if CTFontCopyPostScriptName(runFont) as String != "AppleColorEmoji" { return "fallback_font" }
        var glyphs = [CGGlyph](repeating: 0, count: CTRunGetGlyphCount(run))
        CTRunGetGlyphs(run, CFRange(location: 0, length: 0), &glyphs)
        if glyphs.contains(0) { return "missing_glyph" }
    }
    return nil
}

func context(_ width: Int, _ height: Int, white: Bool) throws -> CGContext {
    let flags = CGBitmapInfo.byteOrder32Big.rawValue | CGImageAlphaInfo.premultipliedLast.rawValue
    guard let result = CGContext(data: nil, width: width, height: height, bitsPerComponent: 8,
                                bytesPerRow: width * 4, space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                bitmapInfo: flags) else { throw Failure.renderFailed }
    if white {
        result.setFillColor(red: 1, green: 1, blue: 1, alpha: 1)
        result.fill(CGRect(x: 0, y: 0, width: width, height: height))
    } else { result.clear(CGRect(x: 0, y: 0, width: width, height: height)) }
    return result
}

func render(_ line: CTLine, color: inout [UInt8]?) throws -> [UInt8] {
    let bounds = CTLineGetBoundsWithOptions(line, .useGlyphPathBounds)
    guard bounds.width > 0, bounds.height > 0, bounds.width.isFinite, bounds.height.isFinite else {
        throw Failure.renderFailed
    }
    let canvas = try context(edge, edge, white: false)
    let scale = CGFloat(edge - 4) / max(bounds.width, bounds.height)
    let x = (CGFloat(edge) - bounds.width * scale) / 2 - bounds.minX * scale
    let y = (CGFloat(edge) - bounds.height * scale) / 2 - bounds.minY * scale
    canvas.translateBy(x: x, y: y)
    canvas.scaleBy(x: scale, y: scale)
    canvas.setAllowsFontSmoothing(false)
    canvas.textPosition = .zero
    CTLineDraw(line, canvas)
    guard let pointer = canvas.data?.bindMemory(to: UInt8.self, capacity: edge * edge * 4) else {
        throw Failure.renderFailed
    }
    if color != nil { color = straightRGBA(pointer) }
    return monochrome(pointer)
}

func straightRGBA(_ pixels: UnsafePointer<UInt8>) -> [UInt8] {
    var result = [UInt8](repeating: 0, count: edge * edge * 4)
    for at in stride(from: 0, to: result.count, by: 4) {
        let alpha = Int(pixels[at + 3])
        if alpha == 0 { continue }
        for channel in 0..<3 {
            result[at + channel] = UInt8(min(255, (Int(pixels[at + channel]) * 255 + alpha / 2) / alpha))
        }
        result[at + 3] = UInt8(alpha)
    }
    return result
}

func boundary(_ pixels: UnsafePointer<UInt8>, _ x: Int, _ y: Int) -> Bool {
    if pixels[(y * edge + x) * 4 + 3] < 128 { return false }
    for (dx, dy) in [(-1, 0), (1, 0), (0, -1), (0, 1)] {
        let nx = x + dx, ny = y + dy
        if nx < 0 || nx >= edge || ny < 0 || ny >= edge { return true }
        if pixels[(ny * edge + nx) * 4 + 3] < 128 { return true }
    }
    return false
}

func luminance(_ pixels: UnsafePointer<UInt8>, _ at: Int) -> Int {
    let white = 255 - Int(pixels[at + 3])
    let r = Int(pixels[at]) + white, g = Int(pixels[at + 1]) + white, b = Int(pixels[at + 2]) + white
    return (54 * r + 183 * g + 19 * b + 128) >> 8
}

func monochrome(_ pixels: UnsafePointer<UInt8>) -> [UInt8] {
    // Preserve dark details; ordered middle tones distinguish colored fills on a one-color head.
    let bayer = [0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5]
    var bits = [UInt8](repeating: 0, count: edge * edge / 8)
    for y in 0..<edge {
        for x in 0..<edge {
            let at = (y * edge + x) * 4
            let threshold = 64 + 12 * bayer[(y & 3) * 4 + (x & 3)]
            if boundary(pixels, x, y) || luminance(pixels, at) < threshold {
                bits[y * edge / 8 + x / 8] |= 0x80 >> (x & 7)
            }
        }
    }
    return bits
}

func bitmap(_ output: Result, _ sequence: String) -> Data? {
    guard let item = output.glyphs.first(where: { $0.sequence == sequence }) else { return nil }
    return Data(base64Encoded: item.bitmap)
}

func writePNG(_ canvas: CGContext, _ path: String) throws {
    guard let image = canvas.makeImage(),
          let target = CGImageDestinationCreateWithURL(URL(fileURLWithPath: path) as CFURL,
                                                       "public.png" as CFString, 1, nil) else {
        throw Failure.renderFailed
    }
    CGImageDestinationAddImage(target, image, nil)
    if !CGImageDestinationFinalize(target) { throw Failure.renderFailed }
}

func preview(_ output: Result, _ path: String) throws {
    let choices = ["😀", "❤️", "👍🏽", "👩‍💻", "🇷🇺", "👨‍👩‍👧‍👦", "🎉", "🖨️"]
    let scale = 5, cell = edge * 5 + 16, columns = 4
    let canvas = try context(cell * columns, cell * 2, white: true)
    canvas.setFillColor(red: 0, green: 0, blue: 0, alpha: 1)
    for (index, sequence) in choices.enumerated() {
        guard let bits = bitmap(output, sequence) else { continue }
        let ox = (index % columns) * cell + 8, oy = (1 - index / columns) * cell + 8
        for y in 0..<edge {
            for x in 0..<edge {
                if bits[y * edge / 8 + x / 8] & (0x80 >> (x & 7)) != 0 {
                    canvas.fill(CGRect(x: ox + x * scale, y: oy + (edge - 1 - y) * scale,
                                       width: scale, height: scale))
                }
            }
        }
    }
    try writePNG(canvas, path)
}

func main() throws {
    let args = CommandLine.arguments
    guard args.count == 4 || (args.count == 5 && args[4] == "--color") else { throw Failure.invalidArguments }
    let colored = args.count == 5
    guard CTFontCopyPostScriptName(font) as String == "AppleColorEmoji" else { throw Failure.invalidFont }
    let input = try FileHandle(forReadingFrom: URL(fileURLWithPath: args[1]))
    defer { try? input.close() }
    guard let data = try input.read(upToCount: 1024 * 1024 + 1), data.count <= 1024 * 1024 else {
        throw Failure.invalidInput
    }
    let requests = try JSONDecoder().decode([String].self, from: data)
    guard !requests.isEmpty, requests.count <= 8192 else { throw Failure.invalidInput }
    var glyphs: [Rendered] = [], unsupported: [Unsupported] = []
    for text in requests {
        guard !text.isEmpty, text.utf8.count <= 64 else { throw Failure.invalidInput }
        let line = makeLine(text)
        if let reason = unsupportedReason(text, line) {
            unsupported.append(Unsupported(sequence: text, reason: reason))
            continue
        }
        var rgba: [UInt8]? = colored ? [] : nil
        let bits = try render(line, color: &rgba)
        if !bits.contains(where: { $0 != 0 }) {
            unsupported.append(Unsupported(sequence: text, reason: "empty_raster"))
            continue
        }
        glyphs.append(Rendered(sequence: text, bitmap: Data(bits).base64EncodedString(),
                               rgba: rgba.map { Data($0).base64EncodedString() }))
    }
    let url = CTFontCopyAttribute(font, kCTFontURLAttribute) as? URL
    let result = Result(postscript: CTFontCopyPostScriptName(font) as String,
                        version: CTFontCopyName(font, kCTFontVersionNameKey) as String? ?? "unknown",
                        fontURL: url?.path ?? "unknown", edge: edge, glyphs: glyphs, unsupported: unsupported)
    try JSONEncoder().encode(result).write(to: URL(fileURLWithPath: args[2]), options: .atomic)
    try preview(result, args[3])
}

do { try main() }
catch { fputs("Apple emoji rasterization failed: \(error)\n", stderr); exit(1) }

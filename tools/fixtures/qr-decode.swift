// Decode the unmodified native PNG through the macOS Vision SDK.
import Foundation
import Vision

var decoded: [String: String] = [:]
for path in CommandLine.arguments.dropFirst() {
    let request = VNDetectBarcodesRequest()
    request.symbologies = [.qr]
    let handler = VNImageRequestHandler(url: URL(fileURLWithPath: path), options: [:])
    try handler.perform([request])
    guard let results = request.results, results.count == 1,
          let payload = results[0].payloadStringValue else {
        fatalError("Expected one readable QR in \(path)")
    }
    decoded[URL(fileURLWithPath: path).lastPathComponent] = payload
}
let data = try JSONSerialization.data(withJSONObject: decoded, options: [.prettyPrinted, .sortedKeys])
FileHandle.standardOutput.write(data)

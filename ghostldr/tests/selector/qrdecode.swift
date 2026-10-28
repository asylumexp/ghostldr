import Foundation
import Vision
import CoreImage
// qrdecode IMAGE...: each QR code's text, one line per image ("-" when none), as macOS's
// Vision reads it: test_selector.py checks the selector's QR codes (selector/src/qr.c)
for path in CommandLine.arguments.dropFirst() {
    guard let img = CIImage(contentsOf: URL(fileURLWithPath: path)) else { print("-"); continue }
    let req = VNDetectBarcodesRequest()
    req.symbologies = [.qr]
    try? VNImageRequestHandler(ciImage: img).perform([req])
    print(req.results?.first?.payloadStringValue ?? "-")
}

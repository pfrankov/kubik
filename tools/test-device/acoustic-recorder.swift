import AVFoundation
import Foundation

// Native capture: AVFoundation's FFmpeg bridge can omit microphone samples.
let args = CommandLine.arguments
let output = URL(fileURLWithPath: args[1])
let stopPath = args[2]
let settings: [String: Any] = [AVFormatIDKey: kAudioFormatLinearPCM,
    AVSampleRateKey: 48000.0, AVNumberOfChannelsKey: 1, AVLinearPCMBitDepthKey: 16,
    AVLinearPCMIsFloatKey: false, AVLinearPCMIsBigEndianKey: false]
let recorder = try AVAudioRecorder(url: output, settings: settings)
recorder.prepareToRecord()
guard recorder.record() else { fatalError("Microphone recording failed") }
let start = ProcessInfo.processInfo.systemUptime
print("recording")
fflush(stdout)
while ProcessInfo.processInfo.systemUptime - start < 120 && !FileManager.default.fileExists(atPath: stopPath) {
    RunLoop.current.run(until: Date().addingTimeInterval(0.05))
}
let audioTime = recorder.currentTime
recorder.stop()
print("audio=\(audioTime) wall=\(ProcessInfo.processInfo.systemUptime - start)")

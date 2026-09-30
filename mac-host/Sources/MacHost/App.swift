import Foundation

typealias Flags = [String: String]

@main
struct MacHostApp {
    static func main() async {
        setvbuf(stdout, nil, _IOLBF, 0) // line-buffered: piped/background runs stay live
        let raw = Array(CommandLine.arguments.dropFirst())
        let command = raw.first ?? "help"
        let flags = parseFlags(raw.dropFirst())
        do {
            switch command {
            case "inspect":
                try await Commands.inspect(flags)
            case "encode-test":
                try await Commands.encodeTest(flags)
            case "capture-test":
                try await Commands.captureTest(flags)
            case "serve":
                try await Commands.serve(flags)
            case "help", "--help", "-h":
                printHelp()
            default:
                FileHandle.standardError.write(Data("unknown command: \(command)\n\n".utf8))
                printHelp()
                exit(2)
            }
        } catch {
            FileHandle.standardError.write(Data("error: \(error.localizedDescription)\n".utf8))
            exit(1)
        }
    }

    static func parseFlags(_ args: some Sequence<String>) -> Flags {
        var out: Flags = [:]
        var it = args.makeIterator()
        while let a = it.next() {
            guard a.hasPrefix("--") else { continue }
            let key = String(a.dropFirst(2))
            if let v = it.next(), !v.hasPrefix("--") {
                out[key] = v
            } else {
                out[key] = "true"
            }
        }
        return out
    }

    static func printHelp() {
        print("""
        mac-host — macOS host agent for the Windows→Mac high-performance remote desktop

        USAGE: mac-host <command> [flags]

        COMMANDS
          inspect                     Print host diagnostics JSON (permissions, HW encoder,
                                      Apple Screen Sharing coexistence, ports, display).
          encode-test                 Encode synthetic frames through VideoToolbox H.264 HW
                                      and report throughput/latency. Verifies the encoder
                                      without needing Screen Recording permission.
                                      flags: --width --height --fps --seconds --bitrate --out
          capture-test                ScreenCaptureKit capture → encode, per-second stats.
                                      Needs Screen Recording permission for the host app
                                      (Terminal) that launches it.
                                      flags: --width --height --fps --seconds --out
          serve                       Run the HP engine: listen on the custom UDP port,
                                      wait for a Windows client, negotiate, stream.
                                      Falls back to the VNC proxy with an explicit reason.
                                      flags: --port --vnc-port --mode [auto|hp|vnc]
                                             --width --height --fps --bitrate
          help
        """)
    }
}

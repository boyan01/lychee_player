import Cocoa
import FlutterMacOS

class MainFlutterWindow: NSWindow {
  override func awakeFromNib() {
    // lib/main.dart reads the command line, without the -NSKey value pairs
    // AppKit and Xcode add.
    let project = FlutterDartProject()
    var arguments: [String] = []
    var skipValue = false
    for argument in CommandLine.arguments.dropFirst() {
      if skipValue {
        skipValue = false
      } else if argument.hasPrefix("-NS") || argument.hasPrefix("-Apple") {
        skipValue = true
      } else {
        arguments.append(argument)
      }
    }
    project.dartEntrypointArguments = arguments
    let flutterViewController = FlutterViewController(project: project)
    let windowFrame = self.frame
    self.contentViewController = flutterViewController
    self.setFrame(windowFrame, display: true)

    RegisterGeneratedPlugins(registry: flutterViewController)

    super.awakeFromNib()
  }
}

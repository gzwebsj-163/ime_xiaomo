import SwiftUI

struct ContentView: View {
    @StateObject private var agent = ClusterAgent()
    @State private var script = """
    void message : str = "Hello, iPhone xiaomo!"
    >> print >> message

    void x : int = 10
    void y : int = 20
    >> print >> ${x} + ${y}
    """
    @State private var output = ""
    @State private var status = "就绪"
    @State private var isRunning = false

    var body: some View {
        TabView {
            scriptView.tabItem { Label("脚本", systemImage: "terminal") }
            agentView.tabItem { Label("Agent", systemImage: "antenna.radiowaves.left.and.right") }
        }
    }

    private var scriptView: some View {
        NavigationView {
            VStack(alignment: .leading, spacing: 10) {
                TextEditor(text: $script)
                    .font(.system(.body, design: .monospaced))
                    .frame(minHeight: 240)
                    .overlay(RoundedRectangle(cornerRadius: 8)
                        .stroke(Color.secondary.opacity(0.3)))

                HStack {
                    Button {
                        runScript()
                    } label: {
                        Label(isRunning ? "运行中" : "运行 .mo", systemImage: "play.fill")
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(isRunning)
                    Spacer()
                    Text(status).font(.footnote).foregroundColor(.secondary)
                }

                Divider()
                ScrollView {
                    Text(output.isEmpty ? "（无输出）" : output)
                        .font(.system(.footnote, design: .monospaced))
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
            .padding()
            .navigationTitle("本机执行")
        }
    }

    private var agentView: some View {
        NavigationView {
            VStack(alignment: .leading, spacing: 12) {
                LabeledContent("Coordinator") {
                    TextField("IP:端口", text: $agent.coordinator)
                        .textInputAutocapitalization(.never)
                        .disableAutocorrection(true)
                }
                TextField("节点名称", text: $agent.nodeName)
                    .textInputAutocapitalization(.never)
                    .disableAutocorrection(true)

                HStack {
                    Button(agent.isRunning ? "停止 Agent" : "启动 Agent",
                           systemImage: agent.isRunning ? "stop.fill" : "play.fill") {
                        agent.isRunning ? agent.stop() : agent.start()
                    }
                    .buttonStyle(.borderedProminent)
                    .tint(agent.isRunning ? .red : .accentColor)
                    Spacer()
                    Text(agent.status).font(.footnote).foregroundColor(.secondary)
                }

                Divider()
                Text("任务日志")
                    .font(.headline)
                ScrollView {
                    Text(agent.log.isEmpty ? "等待启动" : agent.log)
                        .font(.system(.caption, design: .monospaced))
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
            .padding()
            .navigationTitle("手机节点")
        }
    }

    private func runScript() {
        isRunning = true
        status = "运行中"
        let source = script
        Task.detached(priority: .userInitiated) {
            let result = executeMo(source)
            await MainActor.run {
                output = result.output
                status = result.error.map { "失败: \($0)" } ?? "成功"
                isRunning = false
            }
        }
    }
}

private struct LocalMoResult: Sendable {
    let output: String
    let error: String?
}

private func executeMo(_ source: String) -> LocalMoResult {
    let url = FileManager.default.temporaryDirectory
        .appendingPathComponent(UUID().uuidString + ".mo")
    do {
        try source.write(to: url, atomically: true, encoding: .utf8)
    } catch {
        return LocalMoResult(output: "", error: "写文件失败: \(error.localizedDescription)")
    }
    defer { try? FileManager.default.removeItem(at: url) }

    var outputPointer: UnsafeMutablePointer<CChar>? = nil
    var errorBuffer = [CChar](repeating: 0, count: 1024)
    let rc = url.path.withCString { path in
        errorBuffer.withUnsafeMutableBufferPointer { buffer in
            xmo_exec(path, &outputPointer, buffer.baseAddress, Int32(buffer.count))
        }
    }
    var output = ""
    if let pointer = outputPointer {
        output = String(cString: pointer)
        xmo_free(pointer)
    }
    let error = errorBuffer.withUnsafeBufferPointer { String(cString: $0.baseAddress!) }
    return LocalMoResult(output: output,
                         error: rc == 0 ? nil : (error.isEmpty ? "执行失败(\(rc))" : error))
}

struct ContentView_Previews: PreviewProvider {
    static var previews: some View { ContentView() }
}

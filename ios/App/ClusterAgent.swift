import Foundation
import UIKit

@MainActor
final class ClusterAgent: ObservableObject {
    @Published var coordinator = "192.168.0.161:9100"
    @Published var nodeName = ""
    @Published var isRunning = false
    @Published var status = "未连接"
    @Published var log = ""

    private var pollingTask: Task<Void, Never>?

    init() {
        nodeName = "iphone-" + UIDevice.current.name.replacingOccurrences(of: " ", with: "-")
    }

    func start() {
        guard !isRunning else { return }
        isRunning = true
        pollingTask = Task { [weak self] in
            guard let self else { return }
            await self.loop()
        }
    }

    func stop() {
        pollingTask?.cancel()
        pollingTask = nil
        isRunning = false
        status = "已停止"
        appendLog("Agent 已停止")
    }

    private func loop() async {
        appendLog("连接 Coordinator: \(coordinator)")
        while !Task.isCancelled {
            do {
                try await register()
                status = "已在线"
                if let task = try await nextTask() {
                    await execute(task)
                } else {
                    try await Task.sleep(nanoseconds: 10_000_000_000)
                }
            } catch {
                status = "连接失败"
                appendLog(error.localizedDescription)
                try? await Task.sleep(nanoseconds: 5_000_000_000)
            }
        }
    }

    private func register() async throws {
        let processorCount = max(1, ProcessInfo.processInfo.processorCount)
        let memoryMB = max(1, Int(ProcessInfo.processInfo.physicalMemory / 1024 / 1024))
        let payload: [String: Any] = [
            "node_id": nodeName,
            "arch": "iOS-arm64",
            "mem_mb": memoryMB,
            "cores": processorCount,
            "caps": ["mo"],
            "bandwidth": "lan",
            "has_ffmpeg": false,
            "load": 0
        ]
        _ = try await post("/node/register", payload: payload)
    }

    private func nextTask() async throws -> ClusterTask? {
        let data = try await post("/task/next", payload: ["node_id": nodeName])
        let object = try JSONDecoder().decode(ClusterResponse.self, from: data)
        guard object.status == "ok", let id = object.taskID else { return nil }
        return ClusterTask(id: id, code: object.code ?? "")
    }

    private func execute(_ task: ClusterTask) async {
        appendLog("收到任务 \(task.id)")
        let result = await Task.detached(priority: .userInitiated) {
            runMo(task.code)
        }.value

        appendLog(result.error == nil
                  ? "任务 \(task.id) 完成"
                  : "任务 \(task.id) 失败: \(result.error ?? "未知错误")")
        do {
            var payload: [String: Any] = [
                "node_id": nodeName,
                "task_id": task.id,
                "output": result.output
            ]
            if let error = result.error { payload["error"] = error }
            _ = try await post("/task/result", payload: payload)
        } catch {
            appendLog("回传失败: \(error.localizedDescription)")
        }
    }

    private func post(_ path: String, payload: [String: Any]) async throws -> Data {
        let base = coordinator.hasPrefix("http") ? coordinator : "http://\(coordinator)"
        guard let url = URL(string: base + path) else { throw AgentError.invalidAddress }
        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.timeoutInterval = 20
        request.httpBody = try JSONSerialization.data(withJSONObject: payload)
        let (data, response) = try await URLSession.shared.data(for: request)
        guard let http = response as? HTTPURLResponse, (200...299).contains(http.statusCode) else {
            throw AgentError.httpStatus
        }
        return data
    }

    private func appendLog(_ line: String) {
        let stamp = DateFormatter.localizedString(from: Date(), dateStyle: .none, timeStyle: .medium)
        log = ([line].isEmpty ? log : log + "\n[\(stamp)] \(line)").trimmingCharacters(in: .newlines)
        if log.count > 12_000 { log = String(log.suffix(12_000)) }
    }
}

private struct ClusterTask: Sendable {
    let id: String
    let code: String
}

private struct MoResult: Sendable {
    let output: String
    let error: String?
}

private struct ClusterResponse: Decodable {
    let status: String
    let taskID: String?
    let code: String?

    enum CodingKeys: String, CodingKey {
        case status
        case taskID = "task_id"
        case code
    }
}

private enum AgentError: LocalizedError {
    case invalidAddress
    case httpStatus

    var errorDescription: String? {
        switch self {
        case .invalidAddress: return "Coordinator 地址无效"
        case .httpStatus: return "Coordinator 返回 HTTP 错误"
        }
    }
}

private func runMo(_ source: String) -> MoResult {
    let url = FileManager.default.temporaryDirectory
        .appendingPathComponent(UUID().uuidString + ".mo")
    do {
        try source.write(to: url, atomically: true, encoding: .utf8)
    } catch {
        return MoResult(output: "", error: "写入脚本失败: \(error.localizedDescription)")
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
    return MoResult(output: output, error: rc == 0 ? nil : (error.isEmpty ? "执行失败(\(rc))" : error))
}

// ocr_vision.swift — macOS 本地离线 OCR（Vision 框架，零外部 API）
//
// 为什么要有它（2026-09-30）：
//   屏幕渲染 dump 出来的 PNG 要自证「日志区显示的是后端真数据」，
//   但云端视觉 API 全部不可用（豆包欠费 / Kimi key 失效 / OpenAI 无通道）。
//   macOS 自带 Vision 框架的 VNRecognizeTextRequest 完全离线、免费、无 key。
//
// 用法:  swift ocr_vision.swift <image.png>
// 输出:  每行  "x y w h | 识别文本"（归一化坐标，原点左下）

import Foundation
import Vision
import AppKit

let args = CommandLine.arguments
guard args.count > 1 else { print("usage: ocr_vision.swift <image>"); exit(1) }
let url = URL(fileURLWithPath: args[1])
guard let img = NSImage(contentsOf: url),
      let cg = img.cgImage(forProposedRect: nil, context: nil, hints: nil) else {
    print("cannot load image: \(args[1])"); exit(1)
}

let req = VNRecognizeTextRequest()
req.recognitionLevel = .accurate
req.usesLanguageCorrection = false
req.recognitionLanguages = ["en-US", "zh-Hans"]

let handler = VNImageRequestHandler(cgImage: cg, options: [:])
do { try handler.perform([req]) } catch { print("perform error: \(error)"); exit(1) }

let results = req.results ?? []
print("# image \(cg.width)x\(cg.height), \(results.count) text observations")
for obs in results {
    guard let c = obs.topCandidates(1).first else { continue }
    let bb = obs.boundingBox
    print(String(format: "%.3f %.3f %.3f %.3f | %@",
                 bb.origin.x, bb.origin.y, bb.size.width, bb.size.height, c.string))
}

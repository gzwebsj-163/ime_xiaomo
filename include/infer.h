/*
 * infer - kbc 自动推理运行时
 *
 * 加载 .kbc 模型 (examples/intent/intent_kbc.mo → mo2kbc 编译产物),
 * 把输入(文本/语音识别词)经 kvm_io_push 注入 VM;
 * VM 内 .kbc 程序经 feat(i) (FFI 7) 拿到 Q16 特征, 纯字节码 MLP 前向,
 * argmax 出意图并在 VM 内直接打印 INTENT=/ACTION=。
 *
 * 用法:
 *   ./xiaomo infer <model.kbc> text  "打开客厅的灯"   单句推理
 *   ./xiaomo infer <model.kbc> voice <wav>            语音→ASR→意图 全链路
 *   ./xiaomo infer <model.kbc> loop                   交互循环 (Ctrl-D 退出)
 */
#ifndef XIAOMO_INFER_H
#define XIAOMO_INFER_H

int infer_cmd(int argc, char** argv);

#endif

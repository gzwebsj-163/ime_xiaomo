# 魔塔(Modelscope) Notebook · MLP 关键字→图片 生成 + int8 量化训练

> 目标：用 **MLP 条件生成网络**，输入 **关键字** 即可生成对应的 **1024×1024** 图片；
> 对训练好的权重做 **int8 对称量化**（量化训练），验证量化后生成质量（PSNR）。
>
> 方案 R（隐式表示 + 超采样）：
> - **训练 128×128**（每类 16384 像素，GPU 高效）
> - **推理 1024×1024**（批量矩阵乘，隐式坐标→颜色映射平滑放大）
> - **int8 对称量化**（per-tensor scale），导出权重 + scale

## 文件

| 文件 | 说明 |
|---|---|
| `train_imggen_modelscope.py` | 主脚本：造图 → 训练 MLP → int8 量化 → 1024 出图 → 精度对比 |
| `imggen_config.py` | 关键字 / 分辨率 / 网络 / 训练超参 配置 |
| `imggen_quantize.py` | int8 对称量化 + 反量化 + PSNR 工具（独立可复用） |

## 在魔塔 Notebook 运行

```bash
# 建议在魔塔 Notebook 的终端/代码单元执行
pip install modelscope torch torchvision numpy Pillow   # 需要时
python train_imggen_modelscope.py --keywords 山海星空网格火焰 --size 1024 --bits 8
```

## 输出

- `output/{keyword}_float32.png`   —— float32 权重生成的 1024×1024
- `output/{keyword}_int8.png`      —— int8 量化权重生成的 1024×1024
- `output/imggen_quantized.json`   —— int8 量化权重（含 scale）
- 终端打印每类 PSNR（float vs int8）

## 关键字自定义

`--keywords` 逗号分隔即可，任意中/英文字符串。
图案由 `imggen_config.py` 中 `PALETTES` 与 `SHAPES` 决定（可自行增删）。

#!/usr/bin/env python3
"""Canvas 管线图像 I/O — 真实图片载入/推理/保存 全链路

用法:
  python3 run_canvas.py <input.png> <pipeline> [output] [--size WxH]

管线:
  blur        — 3x3 高斯模糊
  edge        — Sobel 边缘检测
  quantize    — Sigmoid 软二值化 (threshold 128)
  overlay     — Overlay 混合 (需两张图: input + second image)
  full        — 组合管线 (blur->edge->quantize)

示例:
  python3 run_canvas.py photo.png blur output.png --size 256x256
  python3 run_canvas.py photo.png edge edges.png
  python3 run_canvas.py a.png overlay b.png --size 128x128
  python3 run_canvas.py photo.png full result.png --size 64x64
"""

import sys
import os
import re
import subprocess
import tempfile
from pathlib import Path

# --- 管线模板 (使用已存在的算子) ---

PIPELINE_TEMPLATES = {
    "blur": """
# Gaussian Blur (3x3)
void k : int = reshape(tensor([[1,2,1],[2,4,2],[1,2,1]]), [1,1,3,3])
void c : int = conv2d(${img}, ${k}, 1, 1, 1, 1)
void out_4d : int = nd_div(${c}, 16.0)
void out_2d : int = reshape(${out_4d}, [{H}, {W}])
void out_arr : int = tensor_to_array(${out_2d})
>> print >> ${out_arr}
""",

    "edge": """
# Sobel Edge Detection
void kx : int = reshape(tensor([[-1,0,1],[-2,0,2],[-1,0,1]]), [1,1,3,3])
void ky : int = reshape(tensor([[-1,-2,-1],[0,0,0],[1,2,1]]), [1,1,3,3])
void gx : int = conv2d(${img}, ${kx}, 1, 1, 1, 1)
void gy : int = conv2d(${img}, ${ky}, 1, 1, 1, 1)
void mag : int = nd_sqrt(nd_add(nd_mul(${gx},${gx}), nd_mul(${gy},${gy})))
void mag_n : int = nd_div(${mag}, 4.0)
void out_2d : int = reshape(${mag_n}, [{H}, {W}])
void out_arr : int = tensor_to_array(${out_2d})
>> print >> ${out_arr}
""",

    "quantize": """
# Sigmoid 软二值化 (threshold=128)
void s_raw : int = nd_div(nd_sub(${img}, 128.0), 10.0)
void s : int = sigmoid_n(${s_raw})
void q : int = nd_mul(${s}, 255.0)
void out_2d : int = reshape(${q}, [{H}, {W}])
void out_arr : int = tensor_to_array(${out_2d})
>> print >> ${out_arr}
""",

    "full": """
# Full pipeline: blur -> edge -> quantize
void k_blur : int = reshape(tensor([[1,2,1],[2,4,2],[1,2,1]]), [1,1,3,3])
void b : int = nd_div(conv2d(${img}, ${k_blur}, 1, 1, 1, 1), 16.0)

void kx : int = reshape(tensor([[-1,0,1],[-2,0,2],[-1,0,1]]), [1,1,3,3])
void ky : int = reshape(tensor([[-1,-2,-1],[0,0,0],[1,2,1]]), [1,1,3,3])
void gx : int = conv2d(${b}, ${kx}, 1, 1, 1, 1)
void gy : int = conv2d(${b}, ${ky}, 1, 1, 1, 1)
void mag : int = nd_sqrt(nd_add(nd_mul(${gx},${gx}), nd_mul(${gy},${gy})))
void mag_n : int = nd_div(${mag}, 4.0)

# sharp sigmoid threshold at 30 (mag_n range 0-255)
void t_raw : int = nd_mul(nd_sub(${mag_n}, 30.0), 5.0)
void t : int = sigmoid_n(${t_raw})
void q : int = nd_mul(${t}, 255.0)
void out_2d : int = reshape(${q}, [{H}, {W}])
void out_arr : int = tensor_to_array(${out_2d})
>> print >> ${out_arr}
""",

    "overlay": """
# Overlay blend using sigmoid soft threshold
void v : int = ${img}
void v2 : int = ${img2}
# soft threshold: w ~ 1 when v > 128, w ~ 0 when v < 128
void w_raw : int = nd_div(nd_sub(${v}, 128.0), 20.0)
void w : int = sigmoid_n(${w_raw})
# lower: (v * v2) / 128
void lower : int = nd_div(nd_mul(${v}, ${v2}), 128.0)
# upper: 255 - (255-v)*(255-v2)/128
void upper : int = nd_sub(255.0, nd_div(nd_mul(nd_sub(255.0, ${v}), nd_sub(255.0, ${v2})), 128.0))
# blend: w * upper + (1-w) * lower
void result : int = nd_add(nd_mul(${w}, ${upper}), nd_mul(nd_sub(1.0, ${w}), ${lower}))
void out_2d : int = reshape(${result}, [{H}, {W}])
void out_arr : int = tensor_to_array(${out_2d})
>> print >> ${out_arr}
""",
}

# --- helpers ---

def load_image(path: str, size=None):
    from PIL import Image
    img = Image.open(path).convert("L")
    if size:
        img = img.resize(size, Image.LANCZOS)
    W, H = img.size
    pix = list(img.getdata())
    rows = [[pix[y * W + x] for x in range(W)] for y in range(H)]
    return rows, H, W

def save_image(data, H, W, path):
    from PIL import Image
    flat = []
    if data and isinstance(data[0], list):
        for row in data:
            flat.extend(v for v in row)
    else:
        flat = list(data)
    flat = [max(0, min(255, int(round(v)))) for v in flat]
    img = Image.new("L", (W, H))
    img.putdata(flat)
    img.save(path)

def parse_last_array(stdout):
    """Parse the last tensor_to_array output (handles multi-line)"""
    # Find the content between the first [[ and the last ]]
    start = stdout.find("[[")
    if start < 0:
        return None
    end = stdout.rfind("]]")
    if end < 0:
        return None
    chunk = stdout[start:end+2]
    # Normalize: join lines, collapse whitespace between list items
    # Remove all newlines
    chunk = chunk.replace("\n", "").replace("\r", "")
    # Remove spaces after commas
    chunk = re.sub(r',\s+', ',', chunk)
    # Remove spaces before/after brackets
    chunk = re.sub(r'\[\s+', '[', chunk)
    chunk = re.sub(r'\s+\]', ']', chunk)
    try:
        import ast
        return ast.literal_eval(chunk)
    except Exception:
        pass
    # More aggressive: remove ALL spaces
    chunk = re.sub(r'\s+', '', chunk)
    try:
        return ast.literal_eval(chunk)
    except Exception:
        return None

def gen_mo(H, W, tensor_rows, template, second=None):
    rows_str = ",\n    ".join(str(r) for r in tensor_rows)
    src = f"# Auto-generated\nvoid raw : int = tensor([\n    {rows_str}\n])\nvoid img : int = reshape(${{raw}}, [1, 1, {H}, {W}])\n"
    if second:
        rows2 = ",\n    ".join(str(r) for r in second)
        src += f"void raw2 : int = tensor([\n    {rows2}\n])\nvoid img2 : int = reshape(${{raw2}}, [1, 1, {H}, {W}])\n"
    src += template
    return src

# --- main ---

def main():
    args = sys.argv[1:]
    if len(args) < 2:
        print(__doc__)
        sys.exit(1)

    input_path = args[0]
    pipeline = args[1].lower()
    output_path = None
    blend_with = None
    size = None

    rest = args[2:]
    i = 0
    while i < len(rest):
        a = rest[i]
        if a.startswith("--size="):
            m = re.match(r"--size=(\d+)x(\d+)", a)
            if m:
                size = (int(m.group(1)), int(m.group(2)))
        elif a == "--size" and i + 1 < len(rest):
            m = re.match(r"(\d+)x(\d+)", rest[i+1])
            if m:
                size = (int(m.group(1)), int(m.group(2)))
            i += 1
        elif a.startswith("--"):
            pass
        elif pipeline == "overlay" and blend_with is None:
            blend_with = a
        elif output_path is None:
            output_path = a
        i += 1

    if output_path is None:
        stem = Path(input_path).stem
        output_path = f"{stem}_{pipeline}.png"

    xiaomo_bin = os.path.expanduser("~/xiaomo/build/xiaomo")
    workdir = os.path.expanduser("~/xiaomo")

    print(f"[Canvas] 载入: {input_path}", file=sys.stderr)
    tensor_rows, H, W = load_image(input_path, size=size)
    print(f"[Canvas] 尺寸: {W}x{H}, 管线: {pipeline}", file=sys.stderr)

    tpl = PIPELINE_TEMPLATES.get(pipeline)
    if tpl is None:
        print(f"[Error] 未知管线: {pipeline}\n  可用: {', '.join(PIPELINE_TEMPLATES.keys())}", file=sys.stderr)
        sys.exit(1)

    tensor2_rows = None
    if pipeline == "overlay":
        if blend_with is None:
            print("[Error] overlay 需要第二张图", file=sys.stderr)
            sys.exit(1)
        tensor2_rows, _, _ = load_image(blend_with, size=(W, H) if size else None)

    tpl_filled = tpl.replace("{H}", str(H)).replace("{W}", str(W))
    mo_src = gen_mo(H, W, tensor_rows, tpl_filled, second=tensor2_rows)

    with tempfile.NamedTemporaryFile(suffix=".mo", prefix="_canvas_", mode="w", delete=False) as f:
        f.write(mo_src)
        tmp_path = f.name

    try:
        print(f"[Canvas] 运行 xiaomo...", file=sys.stderr)
        result = subprocess.run(
            [xiaomo_bin, "run", tmp_path],
            cwd=workdir, capture_output=True, text=True, timeout=30,
        )

        if result.returncode != 0:
            print(f"[Error] xiaomo 返回 {result.returncode}", file=sys.stderr)
            print(result.stderr[:500], file=sys.stderr)
            sys.exit(1)

        data = parse_last_array(result.stdout)
        if data is None:
            print(f"[Error] 无法解析输出", file=sys.stderr)
            print(result.stdout[:1000], file=sys.stderr)
            sys.exit(1)

        save_image(data, H, W, output_path)
        print(f"[Canvas] OK 保存: {output_path}", file=sys.stderr)
        print(output_path)

    finally:
        os.unlink(tmp_path)

if __name__ == "__main__":
    main()
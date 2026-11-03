#!/usr/bin/env bash
# 编译 xiaomo VM 核心为静态库（iOS 设备 / iOS 模拟器 / macOS 本机）
# 用法: ./ios/build_core.sh [ios|sim|mac]   环境变量: NO_BLAS=1 关闭 Accelerate
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${OUT:-$ROOT/ios/build}"
TARGET="${1:-ios}"

case "$TARGET" in
  ios) SDK=iphoneos;        ARCH=arm64;  MIN="-mios-version-min=13.0" ;;
  sim) SDK=iphonesimulator; ARCH="$(uname -m)"; MIN="-mios-version-min=13.0" ;;
  mac) SDK=macosx;          ARCH="$(uname -m)"; MIN="-mmacosx-version-min=11.0" ;;
  *) echo "未知目标: $TARGET（可选 ios / sim / mac）" >&2; exit 2 ;;
esac

SDKPATH="$(xcrun -sdk "$SDK" --show-sdk-path 2>/dev/null || true)"
if [ -z "$SDKPATH" ]; then
  echo "找不到 $SDK SDK。请先安装 Xcode（macOS 13.7 可装 Xcode 15.2）。" >&2
  exit 1
fi

EXTRA=""
[ "${NO_BLAS:-0}" = "1" ] && EXTRA="-DXIAOMO_NO_BLAS"

mkdir -p "$OUT"
OBJS=()
# 核心源码按 C++ 编译（与顶层 Makefile 的 g++ 一致，vm.c 用到了 C++ 语法扩展）
for s in lexer parser ast vm tensor nd_tensor weights; do
  clang++ -x c++ -std=c++17 -c -O2 -I"$ROOT/include" -isysroot "$SDKPATH" -arch "$ARCH" $MIN $EXTRA \
        -o "$OUT/$s.o" "$ROOT/src/$s.c"
  OBJS+=("$OUT/$s.o")
done
# 外壳按 C 编译，导出稳定的 C ABI 供 Swift/ObjC 调用
clang -c -O2 -I"$ROOT/include" -I"$ROOT/ios" -isysroot "$SDKPATH" -arch "$ARCH" $MIN $EXTRA \
      -o "$OUT/xiaomo_embed.o" "$ROOT/ios/xiaomo_embed.c"
OBJS+=("$OUT/xiaomo_embed.o")

LIB="$OUT/libxiaomo_core_$TARGET.a"
rm -f "$LIB"
ar rcs "$LIB" "${OBJS[@]}"
echo "完成: $LIB  (sdk=$SDK arch=$ARCH, NO_BLAS=${NO_BLAS:-0})"

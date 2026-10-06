#!/bin/bash
# 多核心快取一致性 rv32-smp：編譯、跑四核示範、看加速比與一個「bug 博物館」案例、開報告。
# 在 Finder 直接按兩下即可執行。
cd "$(dirname "$0")" || exit 1

echo "=== rv32-smp：多核心快取一致性 RISC-V ==="
echo "工具檢查：verilator / yosys / clang（找不到會中止）"
command -v verilator >/dev/null || { echo "找不到 verilator，請先安裝（brew install verilator）"; exit 1; }
if [ ! -x /opt/homebrew/opt/llvm/bin/clang ] && ! command -v clang >/dev/null; then
  echo "找不到有 RISC-V target 的 clang（brew install llvm lld）"; exit 1
fi

echo
echo "[1/4] 編譯 1/2/4 核模擬器與多核程式（第一次較久）…"
make -s build/vsmp1 build/vsmp2 build/vsmp4 build/vsmp2-bug1 sw || { echo "編譯失敗"; exit 1; }

echo
echo "[2/4] 四核原子計數器壓力測試（同時開 lockstep + 記憶體順序檢查 + 協定模型比對）"
./build/vsmp4 --model --stats build/sw/atomics.elf 2>&1 | grep -E "ok on|races|memory check"

echo
echo "[3/4] 平行 Mandelbrot 的 1→2→4 核加速比與匯流排流量"
make -s bench 2>/dev/null | sed -n '/Mandelbrot/,/^$/p' | sed -n '1,8p'

echo
echo "[4/4] bug 博物館：把「lost upgrade」錯誤放進硬體，看驗證抓到它"
echo "（正確版會通過，這個壞掉的版本應該被記憶體檢查擋下來）"
./build/vsmp2-bug1 --model --max-cycles 4000000 build/sw/lost_upgrade.elf
echo "  ← 上面若印出 MEMORY CHECK FAILED 或 MODEL DISAGREES，就是驗證成功抓到 bug"

echo
echo "產生並開啟 HTML 報告…"
if make -s report >/dev/null 2>&1 && [ -f docs/report.html ]; then
  open docs/report.html
else
  echo "（報告需要完整 build；可稍後執行 make report 再開 docs/report.html）"
fi
echo "完成。"

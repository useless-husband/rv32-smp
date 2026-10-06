# rv32-smp

**一顆具備快取一致性的多核心 RISC-V：N 個循序執行的 RV32IMA 核心（實測 1、2、
4 核），每核各有私有的 MESI L1 資料與指令快取，掛在一條窺探匯流排上，由作者自己
的單核心管線延伸而來。一致性協定用三種會互相對照的方法驗證——窮舉式模型檢查、在
執行中的 RTL 上跑的黃金記憶體順序檢查器、以及 RTL 與抽象模型逐步比對——並且把四
個經典的一致性／原子操作 bug 完整重現出來，每一個都被這套驗證抓到。**

本專案是作者自己單核心處理器
[rv32-pipeline](../自製RISC-V處理器%20rv32-pipeline) 的學習性延伸（從 commit
`dcf1080` 出發：沿用五級 RV32IM 核心 B、它的快取、分支預測器、黃金 ISS、
riscv-tests 測試環境與 Yosys 流程；拿掉浮點單元與單週期核心 A）。多核心 RISC-V
與快取一致性模擬器是常見的課程與開源題目，見[相關研究](#相關研究)。這裡沒有新發
明；重點在於協定由三個必須互相吻合的層次驗證，而且把已知的協定 bug 重現出來——在
模型裡、也在硬體裡——讓你能看到每個 bug「為什麼」會壞。全部都在模擬環境執行；**沒
有** FPGA 開發板，只有 Yosys 的資源估算。

[English README](README.md) · [設計說明](docs/DESIGN.md) ·
[初學者導讀](docs/導讀.zh-TW.md) · [HTML 報告](docs/report.html)（用瀏覽器開啟）

## 內容物

* **`rtl/core.sv`**——一個 hart：五級管線（IF/ID/EX/MEM/WB），含前遞、load-use
  互鎖、BTB＋2-bit 計數器分支預測器、迭代式除法器（都來自 rv32-pipeline），再加上
  **A 延伸指令**（LR.W、SC.W 與九個 AMO）、FENCE，以及每核獨立的 `mhartid` CSR。
* **`rtl/l1d.sv`**——私有 L1 資料快取：2-way、16 bytes 一行、寫回式、寫入配置，採
  **MESI** 協定，含一格寫回緩衝區、LR/SC 的保留（附一段鎖定視窗讓 LR/SC 迴圈能前
  進），以及窺探邏輯。它處理窺探協定必須處理的競態：升級中途失去副本就改成讀取所
  有權、請求碰上還在寫回緩衝區的那一行就由緩衝區回應、遠端寫入會清掉保留。
* **`rtl/bus.sv`**——窺探匯流排：一次只做一筆交易，用輪詢仲裁 2N 個主控（N 個資料
  快取、N 個指令快取），後面接寫回式記憶體。因為交易不重疊，匯流排的順序「就是」每
  一行的一致性順序。
* **`rtl/icache.sv`**——指令快取；指令提取會窺探資料快取（`IFetch` 指令），所以任
  一核心的「寫入後 `fence.i`」都會被每一核的提取看到。
* **`model/mesi.hpp`**——抽象協定，模型檢查器與 RTL 比對檢查共用。
* **`model/mc.cpp`**——帶對稱約化的顯式狀態模型檢查器。
* **`model/rv_iss.c`**——黃金指令集模型（RV32IMA），每核一份。
* **`sim/`**——Verilator 測試框架：黃金記憶體順序檢查器（`memcheck.h`）、RTL↔模型
  比對、以及每核的 lockstep。
* **`sw/runtime/`**——裸機執行環境：多核開機、fork/join（`rt_run_all`）、barrier、
  以及用 AMO 和 LR/SC 兩種方式各寫一版的自旋鎖與號碼牌鎖。

## 結果

以下數字都在本機量測（Apple M5、macOS、與其他工作共用；週期數來自核心自己的計數
器，不受負載影響），指令如表所示。`make test` 會跑 lint、模型檢查、黃金模型的 ISA
測試與完整系統測試，約兩分鐘。

| 項目 | 指令 | 結果 |
|---|---|---|
| 對實作的協定做顯式狀態模型檢查，2–3 個快取 × 1–2 個位址：單寫多讀、資料值、無死結 | `make mc` | 全部性質成立；最大的 3 快取 × 2 位址共 **500,087 個狀態**，1.6 秒 |
| 每個 bug 變體都有最短反例 | `make mc` | 5／5 找到 |
| 官方 riscv-tests（rv32ui＋rv32um＋rv32ua）在 1／2／4 核版本的**每一個 hart** 上執行，與該 hart 的黃金模型 lockstep，並開啟 RTL↔模型比對 | `pytest tests/system/test_riscv_tests.py` | 60 題 ×（1＋2＋4）核 = **420／420 通過** |
| 原子計數器壓力測試（6 種鎖／計數器），1／2／4 核，三重檢查 | `pytest -k coherence` | 通過；無遺失更新 |
| 隨機多核壓力，固定種子、擾動時序，黃金記憶體檢查器＋模型比對 | `pytest -k stress` | 通過（種子 1–8 × 2／4 核） |
| Litmus 測試（MP、SB、LB、2+2W、IRIW、fence 與 AMO 變體），每題 3,000 次，每個觀察到的結果都對照套件附的參考 RVWMO 模型 | `make litmus` | **0 次鬆弛結果**——循序一致、比 RVWMO 更強；所有結果皆被允許 |
| Bug 博物館：每個 bug 用指定程式在 RTL 重現，且在正確版本通過 | `pytest -k museum` | 5／5 抓到、5／5 正確版通過 |
| 對快取控制器與匯流排做變異測試（一行一行改 RTL） | `python tools/mutate.py` | **10／10 變異被殺** |
| Verilator `-Wall`（1／2／4 核與每個 bug 變體） | `make lint` | 乾淨 |
| `yosys check -assert`（無組合迴路、無多重驅動），1／2／4 核 | `make loopcheck` | 乾淨 |

### 加速比（來自 `make bench`，完整表在 [docs/benchmarks.md](docs/benchmarks.md)）

平行 Mandelbrot（把列分給各核；1、2、4 核的校驗和相同，代表做的是同一份工作）：

| 核數 | 週期 | 加速比 | BusRdX | 快取對快取供應的行數 |
|---:|---:|---:|---:|---:|
| 1 | 1,692,930 | 1.00× | 37 | 0 |
| 2 | 852,072 | 1.99× | 2,233 | 2,209 |
| 4 | 429,560 | 3.94× | 3,093 | 3,077 |

一個「大量共享記憶體」的核心（每核用 `amoadd` 去撞一個共享的 16 格直方圖）加速差
很多——2 核 1.46×、4 核 2.02×——因為那些格子在快取間來回彈。另有一個**偽共享**示
範（每核寫自己的計數器，但擠在同一條快取行）比把各計數器分到不同行的版本**慢
2.99×**；`make bench` 會把兩者與失效次數都印出來。

### 合成估算（Yosys，Spartan-7 XC7S50；未佈局繞線）

| 核數 | LUT | 正反器 | Block RAM（36 Kb） | DSP | 邏輯深度 |
|---:|---:|---:|---:|---:|---:|
| 1 | 10,843（33%） | 4,574 | 6 | 4 | 29 層 |
| 2 | 21,668（66%） | 8,839 | 13 | 8 | 30 層 |
| 4 | 43,282 | 17,367 | 26 | 16 | 30 層 |

1 核與 2 核放得進 XC7S50（最大的 Spartan-7）；**4 核放不下**（43k LUT > 32,600），
需要更大的晶片。延遲那一欄是粗略的邏輯層數估算，**不是**時序簽核。見
[synth/report.md](synth/report.md)。

## 三個驗證層次如何互相扣合

1. **模型檢查（離線、窮舉）。** `model/mesi.hpp` 把協定寫成抽象快取／匯流排／記憶體
   上的狀態機（資料抽象成「新鮮／過期」）。`model/mc.cpp` 對小規模設定窮舉每個可達
   狀態，對快取與位址做對稱約化，檢查單寫多讀、資料值不變式、以及前進性（光靠協定
   本身就一定能把未完成的請求做完）。廣度優先讓找到的第一個反例就是最短的。
2. **黃金記憶體檢查器（每次 RTL 執行都跑）。** RTL 在每次存取發生的當下回報它做的
   存取（位址、舊值、新值）。檢查器（`sim/memcheck.h`）依這個執行順序維護一份影子
   記憶體，要求每次讀取都拿到該順序中最新一筆寫入的值、同一週期內不會有兩個快取對
   同一行做衝突的寫入、而且 SC 只有在它的保留顆粒自 LR 以來沒被別的 hart 寫過才成
   功。這個順序就是硬體自己的順序，所以通過也同時證明了機器是循序一致的。
3. **RTL↔模型比對（RTL 加 `--model` 執行時）。** RTL 產生的每個一致性事件——未命
   中、匯流排授權、窺探回應、填入、寫回——都拿去餵 `mesi.hpp`，模型必須允許它並到
   達相同的行狀態。模型不允許的轉移，或結果狀態不同，就讓這次執行失敗。

三個互相獨立又彼此吻合的檢查器正是重點：一個 bug 要同時騙過三個才行。
[Bug 博物館](#bug-博物館)展示當它騙不過時會怎樣。

## Bug 博物館

五個經典的一致性／原子操作 bug，每個都是可切換的 RTL 變體（`-GBUG=k`，見
`rtl/rv_defs.svh`）加一支指定的雙核程式。每個都有模型檢查器給的最短反例、RTL 版本
的重現、以及正確版本的通過。[HTML 報告](docs/report.html)把每個畫成可以讀的時間軸。

| # | bug | RTL 裡被誰抓到 |
|---|---|---|
| 1 | **遺失升級**：一個進行中的 BusUpgr 失去共享副本後沒改成 BusRdX | 黃金記憶體檢查器（讀到過期值） |
| 2 | **寫回競態**：寫回緩衝區不被窺探，使請求讀到過期記憶體 | 黃金記憶體檢查器（讀到過期值） |
| 3 | **LR/SC**：遠端寫入沒清掉保留，使 SC 錯誤地成功 | 黃金記憶體檢查器（SC 原子性） |
| 4 | **錯誤的 E**：讀取未命中在別的快取已有該行時卻拿到 Exclusive | RTL↔模型不吻合（違反單寫多讀） |
| 5 | **永久鎖定**：LR 鎖定視窗永不關閉 | 死結（無前進），由週期上限抓到；模型檢查器的前進性檢查也會標出來 |

## 展示

在 Finder 按兩下 `跑跑看.command`，或：

```
$ make -j4 build/vsmp4 build/sw/atomics.elf
$ ./build/vsmp4 --model --stats build/sw/atomics.elf
amoadd               800 / 800
...
atomics: ok on 4 harts
  memory check: 33980 accesses, 11539 writes, SC 2400 ok / 2 failed
  model agreement: 102645 coherence events checked
```

刻意弄壞的版本（bug 1）被完整抓到：

```
$ ./build/vsmp2-bug1 --model build/sw/lost_upgrade.elf
MEMORY CHECK FAILED: cycle 7263: hart 0 load at 80001754 saw 00000018,
  but the latest write in perform order left 00000019 (hart 1 at cycle 7213)
```

## 編譯與測試

需要 Verilator 5、Yosys、有 RISC-V target 的 clang 與 ld.lld（macOS 上用
Homebrew 的 LLVM，因為 Apple 的 clang 沒有 RISC-V target）、Python 3.10+ 與
C++17 編譯器。第一次執行會把 riscv-tests 以固定 commit 抓到 `build/third_party`；
`make litmus` 另外會把 litmus-tests-riscv 複製到被 gitignore 的 `data/`。

```sh
brew install verilator yosys llvm lld      # macOS
make test        # lint + loopcheck + 模型檢查 + ISA 測試 + 系統測試（約 2 分鐘）
make mc          # 只跑模型檢查器（輸出 build/mc.md）
make bench       # 加速比與一致性流量（輸出 build/bench.md）
make litmus      # litmus 測試對照參考 RVWMO 模型
make museum      # 重新產生 bug 博物館資料
make report      # 靜態 HTML 報告 -> docs/report.html
make synth       # 1／2／4 核的 Yosys 資源估算 -> synth/report.md
python tools/mutate.py           # 對快取控制器與匯流排做變異測試
```

所有指令都從專案根目錄執行；路徑可能含空白或非 ASCII 字元，所以 Makefile 裡的路徑
全是相對路徑。

## 相關研究

多核心 RISC-V 與快取一致性模擬器很常見；這是一個學習專案，站在它們旁邊，不是它們
前面。

* **MIT 6.004 / 6.175 / 6.191 與 6.5900（CS 252 風格）實驗**：多核與一致性是標準教
  材；本專案用 SystemVerilog 照這個進程走。
* **CMU 15-418「窺探式一致性模擬器」／「多核快取模擬器」**：以匯流排上的 MSI/MESI
  建模的課程專題，就像這裡的 `model/`——但它們是週期驅動的模擬器，不驅動真正的 RTL。
* **gem5 的 Ruby / SLICC**：規模與成熟度都遠高的一致性協定規格與檢查。
* **OpenPiton、BlackParrot、Rocket/BOOM＋TileLink、CVA6 多核**：真正可合成、以目錄
  或 TileLink 做一致性的量產級多核 RISC-V。它們是量產規模；這裡是幾百行、為了「讀得
  懂、驗得了」而寫的窺探式 L1。
* **RISC-V litmus 測試（litmus-tests-riscv）與 RVWMO 操作式模型**（Flur、Sarkar、
  Sewell 等）：這裡用到的記憶體模型測試套件與參考結果。本核心循序一致，所以自然滿足
  RVWMO；測試證實這點並顯示它更強。
* **TLA+ / Murφ 的 MESI 模型**：教科書式的一致性協定模型檢查，`model/mc.cpp` 是其中
  一個手寫的小實例（帶對稱約化的顯式狀態搜尋）。

其他核心的數字是用別的工具與規則量的，所以是背景，不是排名。

## 限制

* 只有模擬。沒有上板、沒有時序收斂；Yosys 的延遲數字是邏輯層數估算，不是簽核。4 核
  版本放不進 XC7S50。
* 核心是循序執行，且在單一點以程式順序完成每個記憶體存取（無 store buffer），所以機
  器是**循序一致**的，比 RVWMO 更強。因此 FENCE 在硬體裡是空操作；軟體仍然下正確的
  fence，所以能移植到較弱的機器。
* 只有 RV32IMA：沒有 FPU（在 rv32-pipeline）、沒有 supervisor 模式、沒有中斷、沒有
  MMU、沒有壓縮指令、沒有 `Zacas`（`amocas`）。
* 只有一層快取（私有 L1）；沒有共享 L2、沒有目錄——窺探匯流排就是全部的互連，這也是
  核數不多的原因。目錄與 L2 是未來工作。
* 模型檢查器對小規模設定（最多 3 快取、2 位址）證明的是**抽象**協定，是對設計的強力
  檢查，不是對 Verilog 的證明。RTL↔模型比對在真實執行中把兩者接起來，但不是形式等價
  證明。
* 原子窺探匯流排把所有一致性流量串列化；真正的設計會把它管線化或拆開。

## 授權

MIT（見 [LICENSE](LICENSE)）。riscv-tests（BSD 式授權，加州大學董事會）在編譯時抓
取，不屬於本倉庫；litmus-tests-riscv 套件（`make litmus` 複製到被 gitignore 的
`data/`）有自己的授權，同樣不在此重新散布。

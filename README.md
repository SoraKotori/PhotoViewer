# PhotoViewer

## 章節導覽

- [產品目標](#產品目標)
- [使用方式](#使用方式)
- [圖片呈現品質](#圖片呈現品質)
- [預設參數](#預設參數)
- [系統需求](#系統需求)
- [資源需求](#資源需求)
- [測試環境](#測試環境)
- [驗收條件](#驗收條件)

## 產品目標

- 建立原生 Windows 相片瀏覽器。
- 以每秒 30 張為目標切換 8K 圖片。
- 第一階段優先支援 PNG。
- 初期不加入非必要的複雜功能。

## 使用方式

- 從使用者指定的圖片開始瀏覽同一資料夾中的圖片。
- 使用左方向鍵切換上一張，右方向鍵切換下一張。
- 短按時只切換一張，多次短按依輸入順序逐張完成。
- 長按時依圖片順序逐張切換，放開後停在當前圖片。
- 到達資料夾邊界時停止，不循環。
- 使用 F11 切換無邊框全螢幕，再次按下 F11 還原原視窗狀態。

## 圖片呈現品質

- 使用 Direct2D 高品質縮放。
- 使用 Per-Monitor DPI Awareness V2 避免 DPI 虛擬化；swap-chain back buffer 隨視窗 client area 的實體像素調整，避免 DXGI 額外縮放。

## 預設參數

一般執行使用以下預設值。記憶體上限不限制配置，資源數量由 slot count 限制。所有參數都可由命令列覆寫：

| 參數 | 預設值 | 意義 |
|---|---:|---|
| `--workers=` | 實體核心數，最多 16 | Worker thread 數量 |
| `--compressed-budget-mib=` | 無限制 | 壓縮 PNG Slot Storage 記憶體上限 |
| `--compressed-slot-count=` | 24 | 壓縮 PNG slot 數量上限 |
| `--staging-cache-mib=` | 無限制 | 解碼 staging texture Slot Storage 記憶體上限 |
| `--staging-slot-count=` | 20 | 解碼 staging texture slot 數量上限 |
| `--gpu-cache-mib=` | 無限制 | GPU texture 記憶體上限 |
| `--gpu-reverse-slot-count=` | 1 | 反方向的 GPU texture slots |
| `--gpu-forward-slot-count=` | 5 | 目前方向的 GPU texture slots；包含目前畫面 |

## 系統需求

| 項目 | 最低需求 |
|---|---|
| 作業系統 | Windows 10 version 1703（build 15063）或更新版本，64-bit |
| 處理器 | x86-64-v3 相容處理器 |
| 顯示卡與驅動程式 | 硬體 Direct3D 裝置，Feature Level 11_1 以上並支援 D3D11 fence |
| 執行階段 | Microsoft Visual C++ v14 Redistributable x64，version 14.51.36247 以上 |

## 資源需求

以下以 8K、32-bit RGBA、每秒 30 張為基準；完整像素資料以每張 126.56 MiB、壓縮 PNG 以每張 40 MiB 估算。

依預設 Slot 數量，完整配置容量如下：

| 項目 | Slot 數量 | 每個 Slot | 全部配置時 |
|---|---:|---:|---:|
| 壓縮 PNG 資料 | 24 | 約 40 MiB | 約 960 MiB 系統記憶體 |
| 解碼 staging texture | 20 | 約 126.56 MiB | 約 2531.25 MiB 系統記憶體 |
| GPU texture | 6（1 reverse + 5 forward） | 約 126.56 MiB | 約 759.38 MiB VRAM |

持續切換時的最低頻寬估算如下：

| 項目 | 簡化計算（MiB／秒） | 理論最低頻寬 |
|---|---:|---:|
| 儲存裝置 | 40 × 30 | 1.17 GiB／秒 |
| 系統記憶體 | 2 × (40 + 126.56) × 30 | 9.76 GiB／秒 |
| 獨立顯示卡傳輸介面 | 126.56 × 30 | 3.71 GiB／秒 |

## 測試環境

| 項目 | 規格 |
|---|---|
| CPU | Intel Core i9-13900K，Hyper-Threading 關閉 |
| 記憶體 | DDR5-4800，CL40-40-40-77 |
| GPU | NVIDIA GeForce RTX 5090，driver 591.86 |
| 作業系統 | Windows 11 Enterprise 25H2，build 26200.8875 |
| 儲存裝置 | PCIe 4.0 x4、NVMe 1.4 SSD |
| 編譯器 | MSVC toolset 14.51.36231（compiler 19.51.36252） |

## 驗收條件

效能驗收只覆寫以下參數，其餘沿用預設值：

| 參數 | 驗收值 |
|---|---:|
| `--workers=` | 5 |
| `--compressed-slot-count=` | 8 |
| `--staging-slot-count=` | 8 |
| `--gpu-reverse-slot-count=` | 1 |
| `--gpu-forward-slot-count=` | 2 |

驗收使用 181 張連續且彼此唯一的 7680 × 4320、8-bit RGBA、non-interlaced PNG，採冷啟動並將 warmup 設為 0。第一張作為冷啟動圖片並獨立記錄；後續 180 張圖片的 ready time 必須落在 30 images/s 的理論 deadline 內。

效能改進必須實際降低端到端資源成本；成本在程式、OS、驅動程式與執行緒之間的轉移不視為改進。底層執行緒、I/O、解碼與資源所有權設計統一記錄於 [runtime-design.md](docs/runtime-design.md)。

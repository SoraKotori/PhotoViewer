# PhotoViewer

## 章節導覽

- [產品目標](#產品目標)
- [使用方式](#使用方式)
- [圖片呈現品質](#圖片呈現品質)
- [預設參數](#預設參數)
- [驗收參數](#驗收參數)
- [系統需求](#系統需求)
- [資源需求](#資源需求)
- [執行緒、解碼流程與控制架構](docs/runtime-design.md)

## 產品目標

- 建立原生 Windows 相片瀏覽器。
- 以每秒 30 張為目標切換 8K 圖片。
- 第一階段優先支援 PNG。
- 初期不加入非必要的複雜功能。

## 使用方式

- 從使用者指定的圖片開始瀏覽同一資料夾中的圖片。
- 使用左方向鍵切換上一張，右方向鍵切換下一張。
- 短按時只切換一張，多次短按必須被依序完成，不得遺漏。
- 長按時依序切換，不得跳過中間圖片，放開後停在當前圖片。
- 到達資料夾邊界時停止，不循環。
- 使用 F11 切換無邊框全螢幕，再次按下 F11 還原原視窗狀態。

## 圖片呈現品質

- 使用 Direct2D 高品質縮放。
- 使用 Per-Monitor DPI，並使 back buffer 對應視窗的實體像素，避免 DPI 虛擬化造成 DWM 二次縮放。

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
| `--gpu-texture-slot-count=` | 6 | GPU texture slot 數量上限 |

## 驗收參數

效能驗收只覆寫以下參數，其餘沿用預設值：

| 參數 | 驗收值 |
|---|---:|
| `--workers=` | 5 |
| `--compressed-slot-count=` | 8 |
| `--staging-slot-count=` | 8 |
| `--gpu-texture-slot-count=` | 3 |

效能改進必須實際降低端到端資源成本；成本在程式、OS、驅動程式與執行緒之間的轉移不視為改進。大型資料搬移保持顯式，CPU 密集工作由 Workers 執行，Main Thread 僅負責控制、提交與完成處理。

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
| GPU texture | 6 | 約 126.56 MiB | 約 759.38 MiB VRAM |

持續切換時的最低頻寬估算如下：

| 項目 | 簡化計算（MiB／秒） | 理論最低頻寬 |
|---|---:|---:|
| 儲存裝置 | 40 × 30 | 1.17 GiB／秒 |
| 系統記憶體 | 2 × (40 + 126.56) × 30 | 9.76 GiB／秒 |
| 獨立顯示卡傳輸介面 | 126.56 × 30 | 3.71 GiB／秒 |

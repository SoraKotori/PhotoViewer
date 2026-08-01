# PhotoViewer

原生 Windows PNG 瀏覽器原型，不使用 CMake。專案以 Visual Studio solution／MSBuild 建置，
使用 WIC 解碼 PNG，並以 Direct2D 1.1 呈現。

## 已實作

- 由命令列圖片路徑啟動：`PhotoViewer.exe <image.png>`。
- 掃描同一資料夾內的 PNG，依檔名排序。
- 左右方向鍵切換上一張／下一張，邊界不循環。
- 壓住方向鍵時逐張呈現且不設定固定速率；放開後停在當時已顯示的圖片。
- F11 切換目前螢幕的無邊框全螢幕，第二次 F11 還原原視窗位置與樣式。
- WIC 背景解碼，來源檔案只以 `GENERIC_READ` 開啟。
- 每個作業系統方向鍵 repeat 最多授權前進一張；解碼較慢時保留順序，key-up 會清除未呈現的 repeat 並停在當前圖片。
- 單次按鍵各自保留，依輸入順序逐張呈現；目前顯示索引只在 Direct2D 成功呈現後提交。
- 預設 18 個 WIC decode workers；庫存耗盡時，任一最早空閒的 worker 都可立即接手目前目標，避免等待特定 worker 形成 head-of-line blocking。
- 預讀深度依每張 PNG IHDR 推算的解碼後位元組配置，不再假設所有圖片解析度相同；閒置與單次點按只填 decoded cache，確認持續長按後才加入一個完整 worker pipeline，避免預先解碼後因無空間而被淘汰。
- Workers 依圖片解碼後大小交錯啟動，避免同批完成造成週期性庫存空洞；換圖不會整批清空持久化、索引去重的工作。
- 約 2.25 GiB CPU decoded-image LRU cache。
- WIC 輸出預乘 BGRA，直接建立 Direct2D bitmap，使用 768 MiB LRU cache。
- `ID2D1DeviceContext::DrawBitmap` 搭配 `D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC`。
- flip-discard swap chain、等比例縮放及視窗 resize。
- Per-Monitor DPI Aware V2；swap-chain back buffer 對應視窗的實體 client pixels。
- Direct2D 1.1 device context 綁定 DXGI flip-discard swap chain；不含自訂 shader 或 mipmap 管線。
- named-pipe telemetry，不在圖片資料夾建立 index、thumbnail 或 sidecar。
- 獨立的命令列 acceptance runner，可投遞真正的 Win32 左右鍵訊息。

## 工具鏈

- Visual Studio Build Tools 2026 18.8.2
- MSVC 19.51.36252，toolset 14.51.36231 (`v145`)
- MSBuild 18.8.2.30814
- Windows 11 SDK 10.0.26100.0
- x64
- `/std:c++latest` (`__cplusplus == 202400L`)
- `/permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /W4 /WX /EHsc`

## 建置

```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe" `
    .\PhotoViewer.sln `
    -m `
    -p:Configuration=Release `
    -p:Platform=x64
```

輸出：

- `x64\Release\PhotoViewer.exe`
- `x64\Release\PhotoViewer.Acceptance.exe`

## 執行

```powershell
& .\x64\Release\PhotoViewer.exe `
    "E:\Wuthering Waves\Wuthering Waves Game\Client\Saved\ScreenShot\20260728214052.png"
```

## 最終驗收

```powershell
& .\x64\Release\PhotoViewer.Acceptance.exe `
    --app .\x64\Release\PhotoViewer.exe `
    --source "E:\Wuthering Waves\Wuthering Waves Game\Client\Saved\ScreenShot" `
    --initial 20260728214052.png `
    --scenario final `
    --held-samples 60 `
    --interval 100 `
    --results .\test-results
```

`final` 會執行五分鐘、3,000 次 10 Hz 單次按鍵導航，並另行驗證不限制速率的長按導航；
測試前後會對全部 PNG 計算 SHA-256。
長按預設採集 60 張圖片，先等待 250 ms 初次鍵盤延遲，再以高解析度 timer 每 33 ms 投遞一個 repeat，並在結果目錄輸出
`held-switch-intervals.csv`；可用 `--held-samples <數量>` 與
`--held-initial-delay <毫秒>`、`--held-repeat-interval <毫秒>` 調整測試。摘要把首次間隔記為
`held_initial_gap_ms`，後續 repeat 另以 `held_steady_interval_*` 統計，避免把作業系統預期的首次延遲誤算為穩態卡頓。
只有所有 gate 通過時才會原子產生 `PASS.json`。詳細條件見 `ACCEPTANCE.md`。

## 目前範圍

- 目前僅索引 PNG；WIC 負責 PNG 格式轉換到預乘 BGRA8。
- 尚未加入 ICC／HDR 色彩管理、縮放手勢、檔案刪除或 Windows 預設應用程式註冊。
- 測試資料中的 283 張 PNG 全部是 8-bit RGBA、非 Adam7，但包含 5 種解析度；其中 192 張為 7680×4320。其他 PNG 格式變體仍需額外相容性資料集。

## 可重現的 8K 庫存耗盡測試

以下命令從來源中建立只包含連續同質 PNG 的驗收用 catalog。測試會拒絕任何尺寸、bit depth、color type、compression、filter 或 interlace 不一致的樣本；來源資料夾仍保持唯讀。

```powershell
& .\x64\Release\PhotoViewer.Acceptance.exe `
    --app .\x64\Release\PhotoViewer.exe `
    --source "E:\Wuthering Waves\Wuthering Waves Game\Client\Saved\ScreenShot" `
    --initial 20260621223051.png `
    --homogeneous-catalog `
    --require-inventory-exhaustion `
    --held-initial-delay 0 `
    --held-repeat-interval 5 `
    --decode-workers 18 `
    --held-samples 60 `
    --duration 1 `
    --interval 1000 `
    --results .\test-results
```

固定 catalog 有 74 張 7680×4320、8-bit RGBA、非 Adam7 PNG；5 ms repeat 會讓需求明顯快於正常鍵盤操作，60 張足以跨越 2.25 GiB decoded cache 可容納的約 18 張 8K 圖片。每次結果會保留 `viewer-catalog.txt`、`held-sample-manifest.csv`、`held-switch-intervals.csv` 及庫存遙測事件；指定 `--require-inventory-exhaustion` 時，`summary.json` 的 `held_inventory_empty_samples` 必須大於 0，否則這次執行不算有效的庫存耗盡測試並直接 FAIL。真實操作使用 33 ms；單純需要固定格式樣本、但預期不會耗盡 cache 的相容性測試，只使用 `--homogeneous-catalog`。

## 已通過的完整驗收

`test-results\run-20260801-103036-19208\PASS.json` 記錄 2026-08-01 的 Release 完整驗收：3,000／3,000 request 與 present、283／283 張 decode 與 present 覆蓋、10 張／秒、一般切換 P95 16.683 ms／最大 31.212 ms、長按穩態 P95 50.877 ms／最大 79.443 ms、峰值私有記憶體 4,846.5 MiB、平台成長 0 MiB、GPU cache 767.921 MiB，且來源前後 SHA-256 manifest 完全一致。

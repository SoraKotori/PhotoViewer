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

## 目前範圍

- 目前僅索引 PNG；WIC 負責 PNG 格式轉換到預乘 BGRA8。
- 尚未加入 ICC／HDR 色彩管理、縮放手勢、檔案刪除或 Windows 預設應用程式註冊。


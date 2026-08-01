# PhotoViewer 最終驗收契約

## 來源保護

來源資料夾只能列目錄與讀取。結果目錄不得與來源重疊。PhotoViewer 不得在來源旁建立
thumbnail、index、cache、metadata 或 sidecar。final run 必須在前後對所有來源檔案比對：

- 相對檔名
- 檔案大小
- LastWriteTime
- SHA-256

任一差異均為 FAIL。

## 操作路徑

Acceptance runner 以 `PhotoViewer.exe <完整 PNG 路徑>` 啟動正式 viewer，取得該子程序的
`PhotoViewer.Window` HWND，並以 `WM_KEYDOWN/WM_KEYUP` 投遞 `VK_LEFT`／`VK_RIGHT`。
按鍵必須通過正式 WndProc、catalog、decode pipeline、texture upload 及 Present 路徑。

## 必要階段

1. 指定圖片冷啟動並首次呈現。
2. 驗證 Direct2D high-quality cubic renderer 已啟用。
3. 驗證視窗為 Per-Monitor DPI Aware V2。
4. 驗證 Direct2D bitmap 尺寸等於 PNG IHDR 原始尺寸。
5. 驗證 swap-chain back buffer 尺寸等於實體 client pixels。
6. 投遞 F11，驗證無邊框全螢幕涵蓋目前 monitor 實體範圍；再次 F11 必須還原原視窗。
7. 單次前進及返回，索引與檔名必須正確。
8. 以至少 40 張樣本模擬壓住方向鍵，使用高解析度 periodic timer 每 33 ms 投遞作業系統 repeat；逐一驗證中間索引依序 present、呈現數不得超過首次 keydown 加實際 repeat 數，記錄每次相鄰 present 間隔，並於 key-up 驗證停在當時最後呈現的圖片。
9. 五分鐘、100 ms 間隔的 3,000 次方向鍵導航；到邊界時反轉方向。
10. 全部 283 張至少成功 decode 與 present 一次。
11. 正常關閉子程序。
12. 重新產生完整來源 SHA-256 manifest。

## PASS gates

- 3,000/3,000 鍵盤訊息形成正確的 request。
- 呈現後端必須回報 Direct2D high-quality cubic，不能回退到舊 shader/mipmap renderer。
- DPI context 必須精確為 Per-Monitor V2。
- Direct2D bitmap 尺寸必須等於來源 PNG IHDR；back buffer 必須等於實體 client pixels。
- F11 必須正確進入無邊框全螢幕並完整還原原視窗狀態。
- 壓住期間必須逐張呈現；key-up 後不得繼續呈現已排隊或正在等待的下一張圖片。
- 每個 repeat 最多授權一張；沒有新 repeat 時 viewer 不得自行建立下一個長按目標。
- 必須產生 `held-switch-intervals.csv`，保留每次長按切換的索引、時間戳、間隔及 request-to-present 時間；摘要以 `held_initial_gap_ms` 獨立記錄初次鍵盤延遲，並以 `held_steady_interval_*` 記錄後續 repeat 的樣本數、平均值、P50、P95 與最大間隔。
- 初次長按間隔不得超過設定的 initial delay 加 250 ms；後續穩態 present 間隔 P95 ≤ 100 ms、最大 ≤ 250 ms。不得以降低固定呈現節奏通過此門檻。
- request 順序及索引零錯誤。
- 3,000/3,000 request 都必須形成對應 present，不得漏張。
- 整體呈現速率至少 9.9 張／秒。
- request-to-present：P95 ≤ 100 ms、P99 ≤ 150 ms、最大 ≤ 500 ms。
- 初次呈現 ≤ 3,000 ms。
- decode failure 為 0。
- 全 catalog decode 與 present 覆蓋率為 100%。
- private bytes 峰值 ≤ 5 GiB。
- 中段與末段 private-bytes 中位數增長 ≤ 256 MiB。
- Direct2D bitmap cache ≤ 800 MiB。
- viewer 正常退出且 exit code 為 0。
- 完整來源 manifest 前後一致。

## 可重現的庫存耗盡案例

排程器優化必須分別測試真實 33 ms repeat 與 `--held-repeat-interval 5 --require-inventory-exhaustion` 壓力模式，兩者都使用 `--homogeneous-catalog`，不能從混合解析度 catalog 推論切換間隔。固定案例使用從 `20260621223051.png` 開始的連續 74 張 7680×4320、8-bit RGBA、非 Adam7 PNG，向右長按切換 60 張。

- Viewer 測試程序只可載入結果目錄內 `viewer-catalog.txt` 指定的唯讀來源路徑。
- `held-sample-manifest.csv` 必須逐張記錄檔名、catalog index、壓縮大小、LastWriteTime 與完整 IHDR 格式。
- 61 張測試圖片的寬高、bit depth、color type、compression、filter 與 interlace 必須完全相同，否則立即 FAIL。
- 60 張必須超過 decoded cache 的 8K 容量；runner 必須在 `summary.json` 記錄大於 0 的 `held_inventory_empty_samples`，否則即使其他檢查正常也判定 FAIL。
- 每個候選排程變更至少重複三次相同 manifest，以三次中位數比較平均、P50、P95 與最大間隔；不得挑選單次最好結果。

目前固定 manifest 的 SHA-256 為 `2943AAEDB8E05FB3E502EE8B0B94CC79971C2372A506BE4F9D495BF6C9D5F04D`。三次中位數比較如下；這是 development 診斷結果，不等同 5 分鐘 final 驗收。

| 版本／模式 | 平均間隔 | P50 | P95 | 最大間隔 | 空庫存樣本 | 重複解碼 | private bytes |
|---|---:|---:|---:|---:|---:|---:|---:|
| 舊控制器自行前進／壓力混合基準 | 30.095 ms | 19.638 ms | 116.561 ms | 149.779 ms | 9 | 36 | 3928.55 MiB |
| 任一 worker 接手前景／33 ms 真實操作 | 32.406 ms | 32.663 ms | 43.392 ms | 108.299 ms | 1 | 6 | 3276.81 MiB |
| 任一 worker 接手前景／5 ms 耗盡壓力 | 30.203 ms | 14.569 ms | 156.622 ms | 200.056 ms | 9 | 4 | 3278.16 MiB |

真實模式三次均符合長按 P95／最大間隔門檻；5 ms 模式三次均耗盡，保留分析壓力。表中只列三次中位數，不挑選最好結果。

## 2026-08-01 Release final 結果

證據目錄：`test-results\run-20260801-103036-19208`，完成標記為 `PASS.json`。

- 3,000／3,000 request 與 present；283／283 張皆 decode 與 present。
- 一般 request-to-present：P50 12.074 ms、P95 16.683 ms、P99 23.674 ms、最大 31.212 ms；10 張／秒。
- 長按：首次間隔 244.588 ms；58 個穩態間隔的 P50 32.444 ms、P95 50.877 ms、最大 79.443 ms。
- 峰值 private bytes 4,846.5 MiB；中末段中位數成長 0 MiB；Direct2D bitmap cache 767.921 MiB。
- decode failure 0、wrong request 0、正常退出、完整 283 檔來源 manifest 前後一致。

## 終止條件

開始時建立 `RUNNING.json`。timeout、crash、pipe 中斷或任何 gate 失敗都只能產生
`FAIL.json` 或保留 incomplete 狀態。只有全部必要階段完成後，runner 才以原子 rename
產生 `PASS.json`。沒有 `PASS.json` 就不能宣告任務完成。

# 執行緒、解碼流程與控制架構

## 章節導覽

- [返回 README](../README.md)
- [執行緒與解碼流程](#執行緒與解碼流程)
- [控制架構文字圖](#控制架構文字圖)
  - [關係種類](#關係種類)
  - [基礎設施](#基礎設施)
  - [狀態與資源所有權](#狀態與資源所有權)
  - [共用流程](#共用流程)
  - [事件處理](#事件處理)
  - [Worker 與非同步服務](#worker-與非同步服務)
  - [整體關係](#整體關係)

## 執行緒與解碼流程

- 執行緒組成為一個 Main Thread 與多個 Worker Threads。
- Main Thread 負責事件處理、導航授權、Producer、非同步檔案 I/O 提交與完成處理、工作派發、slot 狀態轉換、GPU 上傳及圖片呈現。
- 事件只來自外部輸入、Windows 與 DXGI 訊號或非同步工作完成。
- Main Thread 每次重新規劃後，依需求範圍與固定記憶體額度批次提交目前可執行的非同步檔案讀取；一般資料夾冷啟動先提交首圖，目錄 I/O 完成後再以 Worker 數量為上限提交相鄰預取，避免同步目錄工作阻塞圖形初始化。
- 一般資料夾開啟時只要求首圖路徑；Main Thread 以該路徑開啟一次檔案，從同一個 handle 取得大小並立刻提交非同步讀取，隨即以 `NtQueryDirectoryFileEx` 提交原生非同步目錄列舉。Windows I/O callback 只送出完成訊息，Main Thread 再解析目錄項目及其 `EndOfFile` 大小、定位排序後的下一張並立即預取。此流程不新增應用程式執行緒、不為大小讀取圖片內容，也不等待、sleep 或輪詢目錄 I/O。
- 壓縮資料、解碼 staging texture 與 GPU texture 都由固定 Slot Storage 持續擁有；管線只傳遞 Slot ID，不移動大型資源所有權。
- 固定大小的 `IoRequest` 由 compressed slot 直接內嵌持有，`WorkToken` 與 worker metrics 依設定數量各做一次連續陣列配置；圖片讀取、解碼派發及 slot 建立不做逐物件 heap allocation。
- 每個 slot 的狀態直接表示該資源目前能被誰使用；可分配索引只收錄狀態為「可分配」的 Slot ID，供 Main Thread 快速取得。
- 提交檔案讀取前取得壓縮 slot。首次取得有效 PNG header 時，Main Thread 立即依寬高取得 staging slot 並建立或重用未映射的 `D3D11_USAGE_STAGING` texture；完整壓縮內容就緒後才映射並派發解碼。首圖與其後圖片使用完全相同的狀態流程。
- 冷啟動在 decoder pool 建立前後及 Direct3D、Direct2D、swap chain、back buffer 各初始化邊界，只處理當下已經完成的 catalog／檔案 I/O；不等待、sleep 或輪詢。若 catalog 在進入 Direct3D 前完成，Main 立即一次派發全部 compressed reservations，使檔案 I/O 與圖形初始化自然重疊。
- Workers 從 Work Queue 取得壓縮 slot 與已映射 staging slot 的 ID，直接在 staging texture 內完成解壓縮與反濾鏡，再將結果及 Slot ID 寫入 Completion Queue。
- Main Thread 收到 Worker 完成結果後解除映射，取得 GPU texture slot，以 `CopySubresourceRegion` 提交非同步複製；GPU fence 完成後 staging slot 才回到「可分配」。
- 元件邊界、狀態所有權、控制與資料流程及架構約束以本文件的[控制架構文字圖](#控制架構文字圖)為準。

## 控制架構文字圖

### 關係種類

```text
事件→ 外部輸入或非同步完成通知，經事件分派器送至固定事件 Handler
讀取→ 使用狀態或資料並保持原值
寫入→ 建立或取代狀態或資料
讀寫→ 讀取後修改同一狀態或資源
擁有→ 將狀態、資源生命週期或共享通道統一收納於單一資源擁有者
計算→ 產生本次同步呼叫使用的暫時選擇或順序，並保持持久狀態原值
判斷→ 依已讀取資料選擇控制分支，並保持持久狀態原值
呼叫→ 事件 Handler 立即執行事件專用流程或共用流程
命令→ 對外部同步系統服務要求立即執行行為
提交→ 啟動外部非同步工作；完成後產生系統事件
```

事件參數攜帶事件來源資料；接收事件的 Handler 從單一資源擁有者讀取其餘判斷所需的狀態與資源。單一資源擁有者保存 Handler 與共用流程使用的全部可變狀態。

### 基礎設施

```text
[基礎設施] 主事件迴圈
│
├─讀取→ Windows 訊息
├─讀取→ 目錄 I/O 完成訊號
├─讀取→ 檔案 I/O 完成訊號
├─讀取→ Worker 完成訊號
├─讀取→ GPU 上傳完成訊號
├─讀取→ DXGI Frame Latency Waitable Object
├─讀寫→ [佇列] 主執行緒事件佇列
└─事件→ [基礎設施] 事件分派器
```

```text
[基礎設施] 事件分派器
│
├─事件：開啟圖片 [外部輸入]────────────→ [事件 Handler] 開啟圖片
├─事件：方向鍵步進 [Windows 輸入]──────→ [事件 Handler] 方向鍵步進
├─事件：方向鍵放開 [Windows 輸入]──────→ [事件 Handler] 方向鍵放開
├─事件：目錄 I/O 完成 [Windows 完成]────→ [事件 Handler] 目錄 I/O 完成
├─事件：PNG header 就緒 [Windows 完成]──→ [事件 Handler] PNG header 就緒
├─事件：檔案 I/O 完成 [Windows 完成]────→ [事件 Handler] 檔案 I/O 完成
├─事件：Worker 工作完成 [Worker 完成]───→ [事件 Handler] Worker 工作完成
├─事件：GPU 上傳完成 [GPU 完成]────────→ [事件 Handler] GPU 上傳完成
├─事件：Frame 提交額度可用 [DXGI 通知]─→ [事件 Handler] Frame 提交額度可用
├─事件：切換全螢幕 [Windows 輸入]──────→ [事件 Handler] 切換全螢幕
└─事件：顯示表面已改變 [Windows 通知]──→ [事件 Handler] 顯示表面已改變
```

事件分派器依事件種類執行固定 Handler 對應；狀態讀取、需求規劃、工作派發及呈現由對應 Handler 執行。

### 狀態與資源所有權

事件所需資源高度耦合，且全部由同一 Main Thread 依明確順序修改，因此所有狀態、資源生命週期與共享通道統一收納在一個資源擁有者。每個事件 Handler 與共用流程都存取這個擁有者；資源擁有者將工作專屬資源實例授予 Worker 與非同步系統服務執行工作。

```text
[資源擁有者] 資源上下文
│
├─擁有→ [狀態] 圖片索引範圍
├─擁有→ [資源] 圖片路徑表
│
├─擁有→ [狀態] 導航授權
│         ├─按住方向
│         ├─最後有效導航方向
│         ├─待完成短按承諾
│         ├─長按 repeat 暫定終點
│         └─目前呈現索引
│
├─擁有→ [狀態] 緩衝需求
│         ├─必須依序呈現範圍
│         ├─解碼預取範圍
│         └─GPU 預取範圍
│
├─擁有→ [狀態] 每張圖片的需求狀態
│         ├─範圍外
│         ├─已要求
│         └─失敗
├─擁有→ [索引] 每張圖片目前關聯的 Slot ID
│         ├─壓縮 PNG Slot ID
│         └─解碼 staging texture Slot ID
│
├─擁有→ [Slot Storage] 壓縮 PNG slots [內容寫入：Windows I/O；內容讀取：Worker]
│         └─[Slot 狀態]
│             ├─可分配
│             ├─正在接收檔案資料
│             ├─可供解碼
│             ├─解碼器正在讀取
│             └─等待目前工作結束後回收
├─擁有→ [可分配索引] 壓縮 PNG Slot ID
├─擁有→ [狀態] 壓縮資料記憶體使用量與預估解碼輸出保留額度
│
├─擁有→ [Slot Storage] 解碼 staging texture slots [內容寫入：取得工作的單一 Worker；內容讀取：GPU]
│         └─[Slot 狀態]
│             ├─可分配
│             ├─已映射且解碼器正在寫入
│             ├─已解碼且可供 GPU 複製
│             ├─GPU 正在讀取
│             └─等待解碼結束後回收
├─擁有→ [可分配索引] 解碼 staging texture Slot ID
├─擁有→ [狀態] staging 記憶體使用量
├─擁有→ [共享通道] Work Queue [Producer：批次填充 Work Queue；Consumers：Workers]
├─擁有→ [共享通道] Completion Queue [Producers：Workers；Consumer：Worker 工作完成 Handler]
│
├─擁有→ [Slot Storage] GPU texture slots [內容寫入：GPU；內容讀取：Direct2D]
│         └─[Slot 狀態]
│             ├─可分配
│             ├─GPU 正在寫入
│             ├─可供 Direct2D 繪製
│             └─等待先前繪製完成
├─擁有→ [可分配索引] GPU texture Slot ID
├─擁有→ [索引] 可呈現 GPU 圖片
│         └─圖片索引 → 可供 Direct2D 繪製的 GPU texture Slot ID
├─擁有→ [狀態] GPU 記憶體使用量
├─擁有→ [狀態] GPU 上傳工作
│         ├─圖片索引
│         ├─解碼 staging texture Slot ID
│         ├─GPU texture Slot ID
│         └─完成值
│
├─擁有→ [狀態] Frame 提交額度
├─擁有→ [狀態] 視窗與全螢幕狀態
└─擁有→ [狀態] 顯示表面實體像素尺寸
```

### 共用流程

圖中未加前綴的狀態與資源都位於同一個 `資源上下文` 內。

#### 重新計算緩衝需求

```text
[共用流程] 重新計算緩衝需求
│
├─讀取→ 圖片索引範圍
├─讀取→ 按住方向
├─讀取→ 最後有效導航方向
├─讀取→ 待完成短按承諾
├─讀取→ 長按 repeat 暫定終點
├─讀取→ 目前呈現索引
│
├─寫入→ 必須依序呈現範圍
├─寫入→ 解碼預取範圍
└─寫入→ GPU 預取範圍
```

輸入狀態、索引邊界及目前呈現位置決定導航授權與緩衝範圍；解碼結果決定圖片進入後續管線的時機。

#### 回收範圍外工作與 slots

```text
[共用流程] 回收範圍外工作與 slots
│
├─讀取→ 必須依序呈現範圍
├─讀取→ 解碼預取範圍
├─讀取→ GPU 預取範圍
├─讀寫→ 每張圖片的需求狀態與關聯 Slot ID
├─讀寫→ Work Queue 的 Producer 端與工作取消旗標
├─讀寫→ 壓縮 PNG slot 狀態與可分配索引
├─讀寫→ 解碼 staging texture slot 狀態與可分配索引
├─讀寫→ GPU texture slot 狀態與可分配索引
├─讀寫→ 可呈現 GPU 圖片索引
├─讀寫→ staging 與 GPU 記憶體使用量
│
├─判斷→ 已要求且尚未取得 slot 的索引離開需求範圍
│   └─寫入→ 需求狀態為範圍外
├─判斷→ 壓縮 slot 正在接收檔案資料且索引已離開需求範圍
│   ├─寫入→ 壓縮 slot 為等待目前工作結束後回收
│   └─命令→ CancelIoEx
├─判斷→ 仍在 Work Queue 且索引已離開需求範圍
│   └─寫入→ 以工作項目的原子旗標取消；只有尚未被 Worker 取得時立即釋放資源
├─判斷→ Worker 已取出且索引已離開需求範圍
│   └─寫入→ 完成後可直接釋放
├─判斷→ 解碼 staging texture 位於保留範圍外且未被 Worker 或 GPU 使用
│   └─寫入→ staging slot 為可分配並加入可分配索引
└─判斷→ 可呈現 GPU 圖片位於保留範圍外且不是目前畫面
    ├─寫入→ 從可呈現 GPU 圖片索引移除圖片
    └─寫入→ 對應 GPU texture slot 等待先前繪製完成後設為可分配並加入可分配索引
```

Slot Storage 在整個工作階段持續擁有實體資源；回收只解除圖片與 Slot ID 的關聯並更新 slot 狀態。可分配索引只供分配時取得可用 Slot ID；分配後的工作提交、完成處理與狀態轉換都直接攜帶 Slot ID。呈現時由可呈現 GPU 圖片索引直接取得 GPU texture Slot ID，不掃描 Slot Storage。

#### 批次提交可執行的檔案讀取

```text
[共用流程] 批次提交可執行的檔案讀取
│
├─讀取→ 必須依序呈現範圍
├─讀取→ 解碼預取範圍
├─讀取→ 圖片路徑表
├─讀取→ 每張圖片的需求狀態與關聯 Slot ID
├─讀寫→ 壓縮 PNG slot 狀態與可分配索引
├─讀取→ 壓縮資料記憶體使用量
├─讀取→ 預估解碼輸出保留額度
├─讀取→ staging 記憶體額度
│
├─計算→ 範圍內已要求且尚未關聯任何 slot 的圖片
├─計算→ 必須呈現的相鄰圖片優先，其次才是預取圖片
├─判斷→ 對每個目前可容納的圖片
│   ├─寫入→ 取得可分配的壓縮 slot 並設為正在接收檔案資料
│   ├─寫入→ 保留預估解碼輸出額度
│   ├─寫入→ 圖片關聯壓縮 Slot ID
│   └─提交→ Windows 非同步檔案 I/O
└─判斷→ 待讀需求已全部提交，或下一筆會超過記憶體或 slot 數量上限時結束
```

一般事件一次提交全部已取得 compressed reservation 且目前可執行的讀取，不另外以 Worker 數量或啟動批次限制截斷。一般資料夾冷啟動在暫時 catalog 只有首圖，因此先提交首圖；目錄 I/O 完成並建立完整 reservation 後，同一次共用流程會派發所有能取得 compressed slot 與記憶體額度的 reservation。固定驗證清單也遵守相同規則。Direct3D、Direct2D、swap chain 與 back buffer 各初始化階段之間，若 I/O 已完成便先填充 Work Queue，讓解碼與剩餘圖形初始化重疊，否則不等待並直接繼續。需求範圍、記憶體上限與壓縮 slot 數量上限共同決定提交集合；任何釋放 slot 或記憶體額度的事件都會再次呼叫本流程。

#### 批次填充 Work Queue

```text
[共用流程] 批次填充 Work Queue
│
├─讀取→ 必須依序呈現範圍
├─讀取→ 解碼預取範圍
├─讀取→ 每張圖片的需求狀態與關聯 Slot ID
├─讀寫→ 壓縮 PNG slot 狀態
├─讀寫→ 解碼 staging texture slot 狀態與可分配索引
├─讀寫→ Work Queue 的 Producer 端
│
├─計算→ 壓縮資料可用且仍需要解碼的圖片
├─計算→ 必須呈現的相鄰圖片優先，其次才是預取圖片
├─判斷→ 對每個壓縮內容與 Prepared staging texture 都已就緒的圖片
│   ├─寫入→ 映射已建立或重用的 staging texture
│   ├─寫入→ 壓縮 slot 為解碼器正在讀取
│   ├─寫入→ 圖片關聯解碼 staging texture Slot ID
│   └─寫入→ Work Queue 工作
│       ├─壓縮 PNG Slot ID
│       └─解碼 staging texture Slot ID
└─判斷→ 可解碼圖片已全部派發、staging texture 全部使用中或 Work Queue 已達安全容量時結束
```

解碼 staging texture slot 通常已在 header 就緒事件中取得並建立為 `Prepared`，本流程只在完整壓縮內容可用後映射；對於 header 已快取的重新載入，本流程也會先走同一個 Prepare helper 再映射。若額度被較低優先的 Prepared 或已解碼 staging 佔滿，先回收最低優先且尚未交由 Worker／GPU 的 staging，再派發下一張必須呈現的圖片。Worker 只依 Work Queue 內的 Slot ID 執行解碼；Main Thread 管理圖片關聯、slot 狀態與可分配索引。

#### 批次提交可執行的 GPU 上傳

```text
[共用流程] 批次提交可執行的 GPU 上傳
│
├─讀取→ 必須依序呈現範圍
├─讀取→ GPU 預取範圍
├─讀取→ 每張圖片的需求狀態與關聯 staging Slot ID
├─讀取→ 可呈現 GPU 圖片索引
├─讀寫→ 解碼 staging texture slot 狀態
├─讀寫→ GPU texture slot 狀態與可分配索引
├─讀寫→ GPU 記憶體使用量
├─讀寫→ GPU 上傳工作
│
├─計算→ 關聯已解碼 staging slot，且不在可呈現 GPU 圖片索引或 GPU 上傳工作中的圖片
├─計算→ 下一張必須呈現的圖片優先，其次依距離排序
├─判斷→ 對每個目前可提交的圖片
│   ├─寫入→ 取得可分配的 GPU texture slot 並設為 GPU 正在寫入
│   ├─寫入→ GPU 上傳工作
│   │   ├─圖片索引
│   │   ├─解碼 staging texture Slot ID
│   │   ├─GPU texture Slot ID
│   │   └─GPU fence 完成值
│   ├─提交→ 以空白初始資料建立 GPU texture
│   ├─提交→ 從 staging texture 複製原始圖片範圍至 GPU texture
│   └─寫入→ staging slot 為 GPU 正在讀取
└─判斷→ 待上傳圖片已全部提交、GPU 記憶體或 slot 數量上限或提交容量已用完時結束
```

GPU texture 建立不傳入初始像素資料。staging texture 在 fence 完成前持續作為非同步複製來源；Main Thread 不等待 GPU，Worker 持續取得其他 staging slot 執行解碼。

#### 嘗試提交下一張畫面

```text
[共用流程] 嘗試提交下一張畫面
│
├─讀取→ Frame 提交額度
├─讀取→ 目前呈現索引
├─讀取→ 待完成呈現授權
├─讀取→ 可呈現 GPU 圖片索引
├─讀取→ 索引直接取得的 GPU texture Slot ID 與 slot 狀態
│
├─判斷→ 呈現授權序列為空時保持目前畫面
├─判斷→ 下一個相鄰索引仍在 GPU 上傳前階段時保持目前畫面並等待該索引
├─判斷→ Frame 提交額度處於等待狀態時等待後續額度事件
└─判斷→ 呈現授權、相鄰 GPU 圖片與 Frame 額度全部可用
    ├─命令→ Direct2D 使用該 GPU texture Slot ID 高品質繪製至實體像素 back buffer
    ├─命令→ DXGI SwapChain Present
    ├─寫入→ 消耗一份 Frame 提交額度
    ├─寫入→ 目前呈現索引
    ├─寫入→ 完成對應呈現授權
    ├─呼叫→ 重新計算緩衝需求
    ├─呼叫→ 回收範圍外工作與 slots
    ├─呼叫→ 批次填充 Work Queue
    ├─呼叫→ 批次提交可執行的 GPU 上傳
    └─呼叫→ 批次提交可執行的檔案讀取
```

每份 Frame 提交額度最多推進一張。重新繪製同一張圖片會保持呈現授權與目前呈現索引原值。

### 事件處理

#### 開啟圖片事件

```text
[事件 Handler] 開啟圖片
│
├─呼叫→ [事件專用] 建立瀏覽工作階段
│   ├─讀取→ 事件攜帶的起始圖片路徑
│   ├─讀取→ 起始圖片的檔案大小中繼資料
│   ├─寫入→ 只含起始圖片的暫時路徑表與管線狀態
│   ├─提交→ 起始圖片的非同步檔案讀取
│   ├─提交→ `NtQueryDirectoryFileEx` 原生非同步目錄列舉
│   ├─寫入→ 暫時呈現索引
│   ├─寫入→ 第一張短按等級的呈現承諾
│   └─寫入→ 按住方向為停止
│
├─呼叫→ 重新計算緩衝需求
├─呼叫→ 提交冷啟動第一批檔案讀取
├─呼叫→ 批次填充 Work Queue
├─呼叫→ 批次提交可執行的 GPU 上傳
└─呼叫→ 嘗試提交下一張畫面
```

一般資料夾工作階段先初始化首圖狀態並立刻提交首圖讀取；完整目錄由原生非同步 I/O 取得。驗證路徑清單模式仍同步建立固定清單。後續呼叫依圖片需求、關聯 Slot ID 與各 slot 狀態決定可立即推進的階段。

#### 目錄 I/O 完成事件

```text
[事件 Handler] 目錄 I/O 完成
│
├─讀取→ `NtQueryDirectoryFileEx` 完成結果與目錄項目 buffer
├─寫入→ 解析每筆 PNG 路徑及 `EndOfFile` 大小
├─判斷→ 尚有項目時提交下一批原生非同步目錄查詢
├─寫入→ 完成時排序路徑表並定位首圖正式索引
├─寫入→ 將首圖既有 I/O 與 slot 關聯移至正式索引
├─寫入→ 初始化其餘圖片的管線狀態與導航範圍
├─呼叫→ 處理已到達但暫存的首圖 I/O 完成
└─呼叫→ 一次提交全部可執行的 compressed reservations
```

此 Handler 只在 Windows I/O completion 到達後執行；沒有計時等待、sleep 或輪詢。合併時保留首圖已配置的壓縮 slot 與真實檔案大小，不重新讀取首圖。

#### 方向鍵步進事件

```text
[事件 Handler] 方向鍵步進
│
├─呼叫→ [事件專用] 更新導航授權
│   ├─讀取→ 方向鍵方向
│   ├─讀取→ 是否為 Windows repeat
│   ├─讀取→ 圖片索引範圍
│   ├─讀寫→ 按住方向
│   ├─判斷→ 非 repeat
│   │   └─寫入→ 增加一張必須完成的短按承諾
│   ├─判斷→ repeat
│   │   └─寫入→ 長按 repeat 暫定終點沿輸入方向移動一張
│   └─判斷→ 每個事件最多增加一張，並將索引限制於有效邊界內
│
├─呼叫→ 重新計算緩衝需求
├─呼叫→ 回收範圍外工作與 slots
├─呼叫→ 批次提交可執行的檔案讀取
├─呼叫→ 批次填充 Work Queue
├─呼叫→ 批次提交可執行的 GPU 上傳
└─呼叫→ 嘗試提交下一張畫面
```

輸入事件決定導航授權；解碼與 GPU 結果決定授權進入可呈現狀態的時機。

#### 方向鍵放開事件

```text
[事件 Handler] 方向鍵放開
│
├─呼叫→ [事件專用] 結束長按授權
│   ├─讀取→ 放開的方向鍵
│   ├─讀取→ 目前呈現索引
│   ├─讀寫→ 按住方向
│   ├─讀寫→ 長按 repeat 暫定終點
│   ├─寫入→ 捨棄尚未呈現的 repeat 授權
│   └─寫入→ 保留待完成短按承諾
│
├─呼叫→ 重新計算緩衝需求
├─呼叫→ 回收範圍外工作與 slots
├─呼叫→ 批次提交可執行的檔案讀取
├─呼叫→ 批次填充 Work Queue
├─呼叫→ 批次提交可執行的 GPU 上傳
└─呼叫→ 嘗試提交下一張畫面
```

#### 檔案 I/O 完成事件

首次讀取某張圖片時，Main Thread 先從檔案 handle 取得 storage sector 資訊，將至少包含 24-byte PNG header 的 prefix 向上提升到 system page／storage I/O granularity。接著以兩個獨立 `OVERLAPPED` 同時提交 `[0, prefix)` 與 `[prefix, EOF)`；兩段不接觸同一個底層 I/O 單位。已取得 header 的圖片再次載入時直接提交單次完整讀取。

```text
[事件 Handler] PNG header 就緒
│
├─讀取→ 壓縮 PNG slot 的前 24 bytes
├─判斷→ PNG signature 與 IHDR 有效
├─寫入→ 圖片寬度、高度、解碼輸出大小與 header 已知狀態
├─讀取→ 圖片的 staging reservation 與固定 staging 記憶體額度
├─寫入→ 取得 staging Slot ID 並關聯圖片／generation
└─命令→ 立即建立或重用未映射的 `D3D11_USAGE_STAGING` texture
    └─寫入→ staging slot 狀態為 Prepared
```

Windows callback 只把 Windows 已提供的 `io_result` 與 transferred bytes 打包進完成訊息，不讀寫 request 狀態。Main Thread 從訊息保存結果與 header 尺寸，並用一般布林欄位合併 prefix 與 body 完成狀態；不再為已完成的 I/O 重複呼叫 `GetOverlappedResult`，完成狀態也不需要跨執行緒 atomic 或 lock。Main Thread 不等待或輪詢 storage I/O；每個已同時具備 header 與完整內容的 request 立即推進 pipeline。texture 建立是 Main Thread 上的 D3D 記憶體準備，與仍在進行的 body storage I/O 互不相依；只有兩段讀取都完成後，壓縮 slot 才成為可解碼狀態。完成後關閉 threadpool I/O registration 前只收合已投遞訊息之 callback 的尾聲，以保證 callback context 生命週期。首圖在只有路徑的單項暫存 catalog 中先走同一個 header → Prepared texture 流程；非同步 catalog 完成並取得正式索引時，同步更新其 I/O、compressed slot 與 staging slot 的圖片索引。Catalog 完成後一次派發全部可執行的 compressed reservations；排序優先級仍使第二張最先提交，但不以人工批次上限省略其餘已授權讀取。

```text
[事件 Handler] 檔案 I/O 完成
│
├─呼叫→ [事件專用] 排空檔案 I/O 完成結果
│   ├─讀取→ 目前全部 I/O 完成結果
│   ├─讀寫→ 每張圖片的需求狀態與關聯壓縮 Slot ID
│   ├─讀寫→ 壓縮 PNG slot 狀態與可分配索引
│   ├─判斷→ 成功且仍需要
│   │   └─寫入→ 壓縮 slot 為可供解碼
│   ├─判斷→ 成功且索引已離開需求範圍
│   │   └─寫入→ 壓縮 slot 為可分配、加入可分配索引，並釋放預估輸出保留額度
│   ├─判斷→ 失敗或取消完成
│   │   └─寫入→ 壓縮 slot 為可分配、加入可分配索引，並釋放預估輸出保留額度
│   └─判斷→ 一次排空目前全部完成結果
│
├─呼叫→ 批次填充 Work Queue
└─呼叫→ 批次提交可執行的檔案讀取
```

成功且保留的壓縮資料持續計入記憶體用量。本次處理釋放額度時，既有等待需求會依新的可用額度進入批次提交判斷。

#### Worker 工作完成事件

```text
[事件 Handler] Worker 工作完成
│
├─呼叫→ [事件專用] 排空 Completion Queue
│   ├─讀取→ Completion Queue 目前全部結果
│   ├─讀寫→ 每張圖片的需求狀態與關聯 Slot ID
│   ├─讀寫→ 壓縮 PNG slot 狀態與可分配索引
│   ├─讀寫→ 解碼 staging texture slot 狀態與可分配索引
│   ├─命令→ 解除完成工作的 staging texture 映射
│   ├─判斷→ 成功且仍需要
│   │   └─寫入→ staging slot 為已解碼且可供 GPU 複製
│   ├─判斷→ 成功且索引已離開需求範圍
│   │   └─寫入→ staging slot 為可分配並加入可分配索引
│   ├─判斷→ 失敗
│   │   └─寫入→ 需求狀態為失敗並釋放 staging slot
│   └─判斷→ 每個完成結果
│       └─寫入→ 釋放已消耗的壓縮資料
│
├─呼叫→ 批次提交可執行的 GPU 上傳
├─呼叫→ 批次填充 Work Queue
├─呼叫→ 批次提交可執行的檔案讀取
└─呼叫→ 嘗試提交下一張畫面
```

Worker 完成事件先解除 staging texture 映射，再依結果轉換 staging slot，並釋放對應的壓縮 slot；所有釋放 slot 或記憶體額度的事件都會呼叫共用 I/O 提交流程。

#### GPU 上傳完成事件

```text
[事件 Handler] GPU 上傳完成
│
├─呼叫→ [事件專用] 排空 GPU 上傳完成結果
│   ├─讀取→ 目前全部 GPU 上傳完成結果
│   ├─讀寫→ GPU 上傳工作
│   ├─讀寫→ GPU texture slot 狀態與可分配索引
│   ├─讀寫→ 解碼 staging texture slot 狀態與可分配索引
│   ├─讀寫→ 可呈現 GPU 圖片索引
│   ├─判斷→ 成功且圖片仍在 GPU 保留範圍
│   │   ├─寫入→ GPU texture slot 為可供 Direct2D 繪製
│   │   ├─寫入→ 可呈現 GPU 圖片索引
│   │   │   └─圖片索引 → GPU texture Slot ID
│   │   └─寫入→ staging slot 為可分配並加入可分配索引
│   ├─判斷→ 成功但圖片已離開 GPU 保留範圍
│   │   ├─寫入→ GPU texture slot 等待先前繪製完成後設為可分配並加入可分配索引
│   │   └─寫入→ staging slot 為可分配並加入可分配索引
│   ├─判斷→ 失敗
│   │   ├─寫入→ GPU texture slot 等待先前繪製完成後設為可分配並加入可分配索引
│   │   └─寫入→ staging slot 為可分配並加入可分配索引
│   └─判斷→ 每個完成結果
│       └─寫入→ 移除對應 GPU 上傳工作
│
├─呼叫→ 批次提交可執行的 GPU 上傳
├─呼叫→ 批次填充 Work Queue
├─呼叫→ 批次提交可執行的檔案讀取
└─呼叫→ 嘗試提交下一張畫面
```

GPU 完成會釋放複製來源 staging slot，讓等待解碼或等待讀取的既有需求重新進入執行判斷；導航需求由輸入事件建立。

#### Frame 提交額度可用事件

```text
[事件 Handler] Frame 提交額度可用
│
├─寫入→ Frame 提交額度可用
└─呼叫→ 嘗試提交下一張畫面
```

呈現流程依相鄰索引順序等待下一張進入 GPU 可用狀態，期間保留導航授權。成功 Present 後所需的需求更新、回收與管線推進，已完整列於 `嘗試提交下一張畫面` 的呼叫圖。

#### 切換全螢幕事件

```text
[事件 Handler] 切換全螢幕
│
└─呼叫→ [事件專用] 切換視窗模式
    ├─讀取→ 視窗與全螢幕狀態
    ├─寫入→ 視窗與全螢幕狀態
    ├─寫入→ 進入全螢幕前的還原資訊
    └─命令→ Windows 視窗系統套用樣式與位置
```

實際顯示表面尺寸以 Windows 後續回報的顯示表面變更事件為準。

#### 顯示表面已改變事件

```text
[事件 Handler] 顯示表面已改變
│
└─呼叫→ [事件專用] 重建顯示表面
    ├─讀取→ Windows 回報的實體像素尺寸
    ├─讀取→ 目前呈現索引
    ├─讀取→ 可呈現 GPU 圖片索引
    ├─寫入→ 顯示表面實體像素尺寸
    ├─命令→ DXGI SwapChain ResizeBuffers
    ├─命令→ 重建 Direct2D back-buffer target
    └─判斷→ 目前圖片對應的 GPU texture slot 可供 Direct2D 繪製且 Frame 提交額度可用
        ├─命令→ Direct2D 使用該 GPU texture Slot ID 重新繪製目前圖片
        ├─命令→ DXGI SwapChain Present
        └─寫入→ 消耗一份 Frame 提交額度
```

重新繪製目前圖片會保持目前呈現索引、呈現授權及圖片讀取需求原值。

### Worker 與非同步服務

```text
[執行者] Worker
│
├─讀寫→ Work Queue 的 Consumer 端與工作取消旗標
├─讀取→ Work Queue 指定的壓縮 PNG slot [Windows 非同步檔案 I/O 寫入]
├─寫入→ Work Queue 指定且已映射的解碼 staging texture slot 像素內容
├─讀寫→ Completion Queue 的 Producer 端
└─事件→ Worker 工作完成 [主事件迴圈]
```

```text
[系統服務] Windows 非同步檔案 I/O
│
├─讀取→ `批次提交可執行的檔案讀取` 同時提交的對齊 prefix/body offset、長度與目的緩衝區
├─寫入→ 同一壓縮 PNG slot 中互不重疊的 prefix/body 範圍
├─事件→ 首次讀取的對齊 prefix 就緒 [主事件迴圈]
└─事件→ body 或單次完整讀取就緒 [主事件迴圈]
```

```text
[執行者] GPU 上傳
│
├─讀取→ GPU 上傳工作指定的解碼 staging texture slot
├─寫入→ GPU 正在寫入的 GPU texture slot
└─事件→ GPU 上傳完成 [主事件迴圈]
```

```text
[系統服務] DXGI SwapChain
│
└─事件→ Frame 提交額度可用 [主事件迴圈]
```

Work Queue 與 Completion Queue 是 Main Thread 和 Workers 之間的共享通道。Work Queue 工作項目的原子旗標協調「尚未取得、已由 Worker 取得、已取消」；Completion Queue 由多個 Worker 寫入、Main Thread 排空。工作只攜帶 Slot ID；Slot Storage 保持資源位址與所有權穩定，slot 狀態授權單一 Worker 在解碼期間讀寫指定資源。

### 整體關係

```text
[基礎設施] 主事件迴圈
│
├─讀取→ Windows 輸入與視窗通知
├─讀取→ Windows I/O、Worker、GPU 與 DXGI 完成訊號
└─事件→ 事件分派器

[基礎設施] 事件分派器
│
└─事件→ 與事件種類固定對應的單一事件 Handler

[事件 Handler]
│
├─讀寫→ [資源擁有者] 資源上下文
├─呼叫→ 事件專用同步流程
├─呼叫→ 共用流程
├─命令→ Windows 視窗系統、Direct2D 與 DXGI SwapChain
└─提交→ Windows 非同步檔案 I/O 或 GPU 上傳

[共用流程]
│
├─讀寫→ [資源擁有者] 資源上下文
├─命令→ 同步系統服務
└─提交→ Windows 非同步檔案 I/O 或 GPU 上傳

[資源擁有者] 資源上下文
│
├─擁有→ 壓縮 PNG、解碼 staging texture 與 GPU texture Slot Storage
├─擁有→ 各類可分配索引與 slot 狀態
├─擁有→ 可呈現 GPU 圖片索引與 GPU 上傳工作
├─擁有→ Work Queue [共享：Workers]
└─擁有→ Completion Queue [共享：Workers]

[執行者與非同步服務] Workers、Windows I/O、GPU 與 DXGI
│
└─事件→ 主事件迴圈
```

架構約束：

- 事件只表示外部輸入或非同步工作完成。
- 每種事件都有一個固定 Handler；事件分派器執行固定對應，Handler 執行狀態讀取、需求規劃、工作派發及呈現。
- 資源上下文是所有 Main Thread 狀態、資源生命週期及共享通道的單一資源擁有者。
- 資源上下文保存事件 Handler 與共用流程使用的全部可變狀態，並由 Main Thread 依序存取。
- Work Queue 與 Completion Queue 是明確標註的共享通道；Producer 與 Consumer 只修改各自的佇列端點及工作項目的原子交接狀態。
- 每個 slot 的使用狀態是資源可否分配的權威；可分配索引只保存狀態為「可分配」的 Slot ID。
- 可分配索引只負責提供可用 Slot ID；分配後的非同步工作、完成結果與狀態轉換直接攜帶 Slot ID，不重新尋找 slot。
- 可呈現 GPU 圖片索引只保存「圖片索引 → 可供 Direct2D 繪製的 GPU texture Slot ID」，供呈現流程直接定位圖片資源。
- 資源擁有者分別以 slot 狀態授予 Windows、單一 Worker 或 GPU 互斥的資源內容寫入期間。
- 每個共用流程必須保留獨立文字圖；事件專用流程必須直接展開在對應 Handler 下。
- 每個索引同時最多存在一條進行中的檔案讀取與解碼流程；同一張圖片取消後依當下索引狀態重新派發。
- 檔案讀取以需求範圍及固定記憶體額度為準；除冷啟動第一批外，一次批次提交全部可執行要求，在途 I/O 數量由提交結果產生。
- Worker 的工作範圍為取得 Work Queue、解碼工作專屬記憶體資料，以及填充 Completion Queue。
- 解碼 staging texture slot 在 header 就緒後可先處於未映射的 `Prepared`；從映射並派發給 Worker 到解碼完成期間由該 Worker 獨占寫入。Main Thread 解除映射並提交 GPU 複製後，slot 持續作為 GPU 來源，直到 fence 完成才成為「可分配」。範圍外、尚未映射的 Prepared slot 可直接回收。
- GPU texture slot 由 Slot Storage 持續擁有；驅逐時解除圖片關聯，等待先前繪製完成後成為「可分配」。
- 呈現授權由輸入事件建立；解碼與 GPU 推測性預取負責提前準備圖片資源。
- 每個有效呈現授權依相鄰索引順序獲得獨立呈現機會。
- 每個 Windows repeat 最多增加一張暫定授權；方向鍵放開時捨棄待呈現的 repeat 並保留待完成短按承諾。
- 每份 Frame 提交額度最多推進一張圖片。

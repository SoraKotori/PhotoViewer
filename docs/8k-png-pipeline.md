# 8K PNG 雙向播放的資源排程設計

## 目錄

- [背景](#背景)
- [核心問題](#核心問題)
  - [中間階段會亂序完成](#中間階段會亂序完成)
  - [較晚顯示的 frame 不能搶光容量](#較晚顯示的-frame-不能搶光容量)
  - [方向切換後，舊方向的工作可能仍在執行](#方向切換後舊方向的工作可能仍在執行)
  - [SourceTexture 的 GPU 傳輸也會亂序](#sourcetexture-的-gpu-傳輸也會亂序)
- [基本解法：固定數量 reservation、亂序執行、有序顯示](#基本解法固定數量-reservation亂序執行有序顯示)
  - [用語定義](#用語定義)
- [reservation 與實體 slot 的關係](#reservation-與實體-slot-的關係)
  - [CompressedBuffer 與 UploadBuffer](#compressedbuffer-與-uploadbuffer)
  - [SourceTexture](#sourcetexture)
- [單向穩態下的資源流轉](#單向穩態下的資源流轉)
  - [各階段獨立維護 reservation](#各階段獨立維護-reservation)
  - [初始指派與逐份改派](#初始指派與逐份改派)
  - [各資源的 slot 狀態](#各資源的-slot-狀態)
    - [CompressedBuffer](#compressedbuffer)
    - [UploadBuffer](#uploadbuffer)
    - [SourceTexture](#sourcetexture-1)
  - [decoder worker、slot 與記憶體容量](#decoder-workerslot-與記憶體容量)
  - [容量不足時的阻塞傳遞](#容量不足時的阻塞傳遞)
- [方向切換前，SourceTexture 應如何安排](#方向切換前sourcetexture-應如何安排)
- [方向切換時的 reservation 改派策略](#方向切換時的-reservation-改派策略)
- [必須考慮亂序 GPU 傳輸](#必須考慮亂序-gpu-傳輸)
  - [可以立即處理的部分](#可以立即處理的部分)
  - [正在 GPU 寫入的部分](#正在-gpu-寫入的部分)
  - [舊方向的 GPU 傳輸完成後](#舊方向的-gpu-傳輸完成後)
- [新方向的 SourceTexture GPU 傳輸也可以亂序](#新方向的-sourcetexture-gpu-傳輸也可以亂序)
- [亂序執行與 deadline 排程](#亂序執行與-deadline-排程)
- [CompressedBuffer 階段的方向切換](#compressedbuffer-階段的方向切換)
  - [尚未開始讀檔](#尚未開始讀檔)
  - [讀檔正在進行](#讀檔正在進行)
  - [已經讀取完成但尚未解碼](#已經讀取完成但尚未解碼)
- [UploadBuffer 階段的方向切換](#uploadbuffer-階段的方向切換)
  - [尚未開始解碼](#尚未開始解碼)
  - [解碼進行中](#解碼進行中)
  - [解碼完成，但 GPU 傳輸尚未提交](#解碼完成但-gpu-傳輸尚未提交)
  - [GPU 傳輸已提交](#gpu-傳輸已提交)
- [完整方向切換流程](#完整方向切換流程)
  - [第一步：設定下一張顯示 frame](#第一步設定下一張顯示-frame)
  - [第二步：重新計算各階段需要的 frame 順序](#第二步重新計算各階段需要的-frame-順序)
  - [第三步：維持仍然有效的 reservation](#第三步維持仍然有效的-reservation)
  - [第四步：立即改派尚未開始工作的 reservation](#第四步立即改派尚未開始工作的-reservation)
  - [第五步：讓已開始的操作安全完成](#第五步讓已開始的操作安全完成)
  - [第六步：允許新方向工作亂序完成](#第六步允許新方向工作亂序完成)
  - [第七步：畫面提交嚴格依新方向執行](#第七步畫面提交嚴格依新方向執行)
- [如何維持 30 FPS](#如何維持-30-fps)
  - [準備足夠的逆向 frame](#準備足夠的逆向-frame)
  - [長期吞吐量必須高於 30 張／秒](#長期吞吐量必須高於-30-張秒)
  - [必須看連續具有 Readable SourceTexture 的 frame 數量](#必須看連續具有-readable-sourcetexture-的-frame-數量)
- [反覆切換 100、99、100、99](#反覆切換-1009910099)
- [為什麼這個方法有效](#為什麼這個方法有效)
  - [reservation 數量永遠等於 slot 數量](#reservation-數量永遠等於-slot-數量)
  - [已經開始的工作不會被錯誤重用](#已經開始的工作不會被錯誤重用)
  - [較早 frame 的容量受到保證](#較早-frame-的容量受到保證)
  - [允許所有中間階段保持亂序](#允許所有中間階段保持亂序)
  - [能利用 SourceTexture 尚未被覆寫的內容](#能利用-sourcetexture-尚未被覆寫的內容)
  - [能正確處理亂序 GPU 傳輸](#能正確處理亂序-gpu-傳輸)
- [設計原則總結](#設計原則總結)

## 背景

系統需要連續播放 8K PNG 圖片，規格如下：

* 影像尺寸：7680 × 4320
* 播放速度：30 FPS
* PNG 壓縮資料：約 45 MiB／張
* 解碼後 BGRA 資料：約 126.56 MiB／張
* 約 12 個解碼 worker 平行解碼
* 最終顯示順序必須完全符合播放控制所要求的順序

基本 pipeline 為：

```text
PNG 檔案
    ↓ 檔案讀取
CompressedBuffer
    ↓ 解碼
UploadBuffer
    ↓ GPU 傳輸
SourceTexture
    ↓ 畫面繪製
BackBuffer
    ↓ 畫面提交
顯示器
```

讀檔、解碼與 GPU 傳輸都可能亂序完成。例如 Frame 105 可能比 Frame 102 更早完成 GPU 傳輸。

單向播放時，顯示順序可能是：

```text
100 → 101 → 102 → 103 → ...
```

系統還需要支援播放途中切換方向，例如：

```text
100 → 99 → 98 → 97
```

甚至反覆切換：

```text
100 → 99 → 100 → 99 → 100 → 99
```

方向切換只改變下一張應顯示的 frame，不代表需要重新建立整條 pipeline，也不代表同一個 frame 每次顯示都要重新讀取、解碼與上傳。

---

## 核心問題

系統同時要解決四個問題。

### 中間階段會亂序完成

Frame 104 可能比 Frame 101 更早完成讀檔、解碼或 GPU 傳輸。

如果強制所有階段都依顯示順序執行，就會失去多 worker 平行處理的效果。

因此中間階段必須允許：

```text
亂序開始
亂序完成
亂序回收資源
```

---

### 較晚顯示的 frame 不能搶光容量

假設 UploadBuffer 有 8 個 slots。

如果較晚顯示的 Frame 105～108 先完成讀檔，並占用了大部分 UploadBuffer，Frame 101～104 隨後完成時可能沒有足夠容量同時進入解碼。

pipeline 可能退化成：

```text
Frame 101 解碼完成
→ Frame 102 才能開始
→ Frame 103 才能開始
```

所以需要 reservation 先保證容量，避免較晚顯示的 frame 搶走較早顯示的 frame 所需要的 slot。

---

### 方向切換後，舊方向的工作可能仍在執行

例如播放到 Frame 100 時，系統原本正在準備：

```text
101、102、103、104...
```

使用者突然切換成反向，新的需求變成：

```text
99、98、97、96...
```

但此時可能存在：

* 某些讀檔尚未開始
* 某些讀檔正在進行
* 某些解碼 worker 正在寫入 UploadBuffer
* 某些 GPU 傳輸已經提交
* 某些 SourceTexture 正在被畫面繪製使用

尚未開始的工作可以重新排程；已經開始的 CPU 或 GPU 操作則不能假裝已經消失，必須等目前操作安全結束。

---

### SourceTexture 的 GPU 傳輸也會亂序

即使新的 SourceTexture reservation 順序是：

```text
99、98、97、96、95
```

實際完成順序仍可能是：

```text
95 → 97 → 96 → 99 → 98
```

所以 SourceTexture 可能出現：

```text
99：SourceTexture 不是 Readable
98：SourceTexture 是 Readable
97：SourceTexture 是 Readable
96：SourceTexture 是 Readable
95：SourceTexture 是 Readable
```

雖然已有四張完成，但仍不能顯示 98，因為下一張必須是 99。

因此系統不能只統計 `Readable` SourceTexture 的總數，而必須關心：

> 從下一張應顯示的 frame 開始，有多少個連續 frame 具有 `Readable` SourceTexture。

---

## 基本解法：固定數量 reservation、亂序執行、有序顯示

每個資源階段都有固定數量的實體 slots：

```text
CompressedBuffer slots
UploadBuffer slots
SourceTexture slots
```

### 用語定義

* `reservation`：某一個 slot 的容量已指派給某個 frame
* `指派`：將 reservation 對應到 frame
* `改派`：將 reservation 從一個 frame 改給另一個 frame
* `順向 frame`：沿目前播放方向繼續播放時，接下來會顯示的 frame
* `逆向 frame`：切換播放方向後，接下來會顯示的 frame

reservation 的數量始終固定。播放期間只會指派或改派，不會建立第二批 reservation。

對容量為 `N` 的階段，系統維持恰好 `N` 份 reservation。

更精確地說，它是一張固定長度的 reservation 表：

```text
reservation[0 ... N-1]
```

每一份 reservation 代表：

> 該階段的一份容量已經保證給某個 frame。

因此：

```text
reservation 數量 = slot 數量
```

不是：

```text
正在使用的 slot
+
額外的 reservation
```

正在執行、等待執行與已經完成的 frame，都包含在這 `N` 份 reservation 之中。

---

## reservation 與實體 slot 的關係

### CompressedBuffer 與 UploadBuffer

在 CompressedBuffer 與 UploadBuffer 階段，reservation 不一定要預先綁定某一個實體 slot。

例如 UploadBuffer 有 8 個 slots，目前 reservation 是：

```text
101、102、103、104、105、106、107、108
```

其中可能只有：

```text
101、104、106
```

已經實際取得 UploadBuffer，其餘只是持有容量保證。

只要維持：

```text
持有 reservation 的 frame 數量 = 8
```

reservation 已指派給某個 frame 後，該 frame 進入此階段時就一定能取得實體 slot，不會被其他 frame 搶走容量。

---

### SourceTexture

SourceTexture 比前兩個階段多一個問題：實體 texture 內可能仍有前一次寫入的 frame，而且 texture 可能正在被 GPU 讀取或寫入。

因此每個 SourceTexture slot 必須同時記錄三種資訊：

```text
reservedFrame
contentFrame
slotState
```

例如：

```text
reservedFrame = 105
contentFrame  = 97
slotState     = Writable
```

這代表：

* 該 slot 的 reservation 已指派給 Frame 105
* texture 目前實際內容仍是 Frame 97
* GPU 尚未開始把它覆寫成 Frame 105

Frame 97 不占用另一份資源。它只是該 texture 在這次改派完成覆寫前的現有內容。

---

## 單向穩態下的資源流轉

### 各階段獨立維護 reservation

CompressedBuffer、UploadBuffer 與 SourceTexture 各自維護獨立的 reservation 表。

reservation 不會從一個階段移動到下一個階段。同一個 frame 在跨階段交接時，可以同時出現在多張 reservation 表中。例如解碼進行中時，該 frame 可能同時持有 CompressedBuffer 與 UploadBuffer 的 reservation。

若下一階段的 reservation 尚未指派給該 frame，資料會留在目前階段，並繼續占用目前的實體 slot，直到下游容量可用。

---

### 初始指派與逐份改派

以容量為 8 的 CompressedBuffer 或 UploadBuffer 階段為例。若目前顯示順序是：

```text
Frame 1 → Frame 2 → Frame 3 → ...
```

初始 reservation 依顯示順序指派給：

```text
Frame 1、2、3、4、5、6、7、8
```

不能在 Frame 3 尚無 reservation 時，先將容量指派給 Frame 10。

若 Frame 5～8 較早完成上一階段，它們可以先取得四個實體 slots 並開始工作。Frame 1～4 的 reservation 仍然保證另外四個 slots 的容量，因此四張同時完成上一階段時，都能進入目前階段。

Frame 5～8 完成目前階段，且相關實體資源可以安全重用後，它們的 reservation 可以逐份改派給：

```text
Frame 9、10、11、12
```

此時 reservation 集合可能是：

```text
Frame 1、2、3、4、9、10、11、12
```

reservation 集合不必是連續區段，也不必等待最早顯示的 frame 完成後整批前移。只需維持：

```text
reservation 的指派順序 = 顯示需求順序
工作的完成順序 = 可以亂序
reservation 的改派時點 = 原實體資源可安全重用
```

SourceTexture 的目標集合另受雙向播放範圍限制，配置方式從下一節開始說明。

---

### 各資源的 slot 狀態

| 階段 | 實體 slot 保存的內容 | 開始占用 slot | 可以安全重用 slot |
| --- | --- | --- | --- |
| CompressedBuffer | 壓縮 PNG 資料 | 開始讀檔前 | 解碼不再需要壓縮資料後 |
| UploadBuffer | 解碼後的像素資料 | 解碼開始寫入前 | GPU 傳輸完成後 |
| SourceTexture | GPU 上的 frame 內容 | reservation 綁定該 slot 時 | frame 離開 SourceTexture reservation 範圍，且 GPU 不再讀寫該 slot 後 |

CompressedBuffer、UploadBuffer 與 SourceTexture 統一使用四種狀態：

| 狀態 | 抽象定義 |
| --- | --- |
| `Writable` | 下一個合法操作是寫入，但尚未開始 |
| `Writing` | 上游操作正在寫入 slot |
| `Readable` | 寫入已完成，內容可供下游讀取 |
| `Reading` | 下游操作正在讀取 slot |

「完成」是狀態轉移事件，不是額外狀態。

#### CompressedBuffer

| 狀態 | 在此階段的語意 |
| --- | --- |
| `Writable` | 可以開始讀取 PNG 檔案，將壓縮資料寫入 buffer |
| `Writing` | 檔案讀取正在寫入壓縮資料 |
| `Readable` | 檔案讀取完成，壓縮資料有效，等待解碼 |
| `Reading` | 解碼操作正在讀取壓縮資料 |

正常流轉：

```text
Writable
→ 檔案讀取開始
→ Writing
→ 檔案讀取完成
→ Readable
→ 解碼開始
→ Reading
→ 解碼不再需要壓縮資料
→ Writable
```

#### UploadBuffer

| 狀態 | 在此階段的語意 |
| --- | --- |
| `Writable` | 可以由解碼操作寫入像素資料 |
| `Writing` | 解碼操作正在寫入像素資料 |
| `Readable` | 解碼完成，像素資料有效，等待 GPU 傳輸 |
| `Reading` | GPU 傳輸正在讀取像素資料 |

正常流轉：

```text
Writable
→ 解碼開始
→ Writing
→ 解碼完成
→ Readable
→ GPU 傳輸開始
→ Reading
→ GPU 傳輸完成
→ Writable
```

#### SourceTexture

| 狀態 | 在此階段的語意 |
| --- | --- |
| `Writable` | 現有內容不符合 reservation，可以開始寫入目標 frame |
| `Writing` | GPU 傳輸正在寫入目標 frame |
| `Readable` | 內容符合 reservation，可以供畫面繪製讀取 |
| `Reading` | 畫面繪製正在讀取 texture |

正常流轉：

```text
Writable
→ GPU 傳輸開始
→ Writing
→ GPU 傳輸完成
→ Readable
→ 畫面繪製開始
→ Reading
```

畫面繪製完成後有兩種轉移：

```text
Reading
→ 保留目前 frame
→ Readable
```

或：

```text
Reading
→ reservation 改派給其他 frame
→ Writable
```

如果 texture 尚未開始寫入，而方向切換後的 reservation 恰好等於現有內容，則可以直接轉移：

```text
Writable
→ reservation 改為 contentFrame
→ Readable
```

不需要重新執行 GPU 傳輸。

方向切換可能使正在準備的結果失去用途。操作仍須安全完成，但完成後可以直接回到 `Writable`：

```text
Writing
→ 操作完成，但結果已不再需要
→ Writable
```

尚未開始下游讀取的結果也可以直接捨棄：

```text
Readable
→ 結果不再需要
→ Writable
```

因此，寫入完成、讀取完成、重用、改派與結果失效都是事件或動作，不是第五種 slot 狀態。

三種資源的正常生命週期可以概括為：

```text
CompressedBuffer
Writable → Writing → Readable → Reading → Writable

UploadBuffer
Writable → Writing → Readable → Reading → Writable

SourceTexture
Writable → Writing → Readable → Reading
Reading → Readable（保留目前 frame）
Reading → Writable（改派後等待寫入其他 frame）
```

CompressedBuffer 與 UploadBuffer 可以依實際完成順序亂序重用。SourceTexture 必須配合顯示順序與雙向播放範圍，因此通常持有較久。

BackBuffer 由畫面提交機制管理，不屬於上述三張 reservation 表。

---

### decoder worker、slot 與記憶體容量

decoder worker 是執行解碼工作的單元，不是長期保存 frame 的資源。frame 不需要預先綁定特定 worker，worker 完成一張後即可處理另一張。

需要 reservation 的是會跨越非同步階段並持續占用容量的 CompressedBuffer、UploadBuffer 與 SourceTexture。

slot 數量必須與總記憶體容量一起決定。例如 16 個 CompressedBuffer、每張壓縮資料約 45 MiB，最壞情況約需要：

```text
16 × 45 MiB = 720 MiB
```

UploadBuffer 與 SourceTexture 每個約需 126.56 MiB。容量規劃至少要滿足：

```text
CompressedBuffer 容量 ≥ slot 數量 × 單張壓縮資料上限
UploadBuffer 容量     ≥ slot 數量 × 126.56 MiB
SourceTexture 容量    ≥ slot 數量 × 126.56 MiB
```

UploadBuffer 的數量通常需要高於同時工作的 decoder worker 數量，因為部分 UploadBuffer 可能已完成解碼，但仍在等待 GPU 傳輸。reservation 數量不能超過實際記憶體容量能支持的 slot 數量。

---

### 容量不足時的阻塞傳遞

SourceTexture 受顯示順序限制，通常是最容易形成阻塞的階段。若沒有 SourceTexture reservation 可以指派給已解碼的 frame，阻塞會依序向上游傳遞：

```text
SourceTexture 無法改派
→ 已解碼 frame 繼續占用 UploadBuffer
→ decoder worker 無法取得新的 UploadBuffer
→ 已讀取 frame 繼續占用 CompressedBuffer
→ 暫停發出新的讀檔工作
```

這是固定容量 pipeline 的正常流量控制。上游不得繞過 reservation 繼續建立無界限工作，否則固定 slot 數量將無法限制記憶體使用量。

---

## 方向切換前，SourceTexture 應如何安排

若要求在任意時間切換方向後仍能維持 30 FPS，就不能把全部 SourceTexture reservation 都指派給順向 frame。

假設目前顯示 Frame `P`，SourceTexture 共 `N` 個 slots，應將 reservation 分成：

```text
逆向 frame
+
目前顯示中的 frame
+
順向 frame
```

例如共有 8 個 SourceTexture：

```text
Frame 97
Frame 98
Frame 99
Frame 100
Frame 101
Frame 102
Frame 103
Frame 104
```

這八個全部都是正式的 SourceTexture reservation：

```text
reservation = {97, 98, 99, 100, 101, 102, 103, 104}
```

Frame 97～99 是逆向 frame，仍有有效的 reservation。

Frame 101～104 則是順向 frame。

每顯示一張，reservation 範圍向目前方向移動一格。例如顯示 Frame 101 後，可變成：

```text
98、99、100、101、102、103、104、105
```

此時：

* Frame 97 的 reservation 改派給 Frame 105
* 其他七份 reservation 保持不變

穩態播放時，每顯示一張只需要把一份 reservation 改派給新的順向 frame。

---

## 方向切換時的 reservation 改派策略

假設目前剛顯示完 Frame 100，方向原本是正向。

SourceTexture 的 8 個 reservation 為：

```text
97、98、99、100、101、102、103、104
```

切換成反向後，下一段所需要的 reservation 變成：

```text
96、97、98、99、100、101、102、103
```

也就是：

```text
不變：97～103
改派：104 → 96
```

方向切換不是額外建立另一組 reservation。

整個過程中永遠維持：

```text
SourceTexture reservation 數量 = 8
```

只把 reservation 從不再需要的 frame 逐步改派給新方向的 frame。

---

## 必須考慮亂序 GPU 傳輸

假設切換方向前的實際 SourceTexture 狀態如下：

| Slot | reservation | 目前內容 | 狀態            |
| ---- | ----------: | ---: | ------------- |
| S0   |          97 |   97 | `Readable` |
| S1   |          98 |   98 | `Readable` |
| S2   |          99 |   99 | `Readable` |
| S3   |         100 |  100 | `Reading`  |
| S4   |         101 |  101 | `Readable` |
| S5   |         102 |   95 | `Writable` |
| S6   |         103 |   96 | `Writable` |
| S7   |         104 |   94 | `Writing`  |

切換成反向後，目標 reservation 是：

```text
96、97、98、99、100、101、102、103
```

### 可以立即處理的部分

S6 尚未開始覆寫，而且目前內容正好是 Frame 96：

```text
改派前：Frame 103
目前內容：96
```

可以立即將它改成：

```text
改派後：Frame 96
目前內容：96
狀態：Readable
```

不需要重新解碼，也不需要重新執行 GPU 傳輸。

因此 Frame 103 暫時沒有 reservation。

---

### 正在 GPU 寫入的部分

S7 正在把 texture 寫成 Frame 104：

```text
reservation：104
GPU 傳輸已提交
```

在 GPU 傳輸完成前，不能把 S7 改成 Frame 103，也不能把它視為可供新方向使用的空 slot。

所以切換瞬間的 reservation 是：

```text
96、97、98、99、100、101、102、104
```

總數仍是 8。

Frame 103 此時只是：

```text
下一個等待指派 SourceTexture reservation 的 frame
```

它尚未進入 reservation 表。

---

### 舊方向的 GPU 傳輸完成後

當 S7 的 Frame 104 GPU 傳輸完成：

```text
S7 的內容變成 104
```

Frame 104 已不再需要，因此立即改派這份 reservation：

```text
104 → 103
```

此時 reservation 才變成：

```text
96、97、98、99、100、101、102、103
```

仍然是 8 個。

若有多個舊方向 GPU 傳輸正在進行，它們可能亂序完成。

正確規則是：

> 每當一份舊方向 reservation 可以安全改派，就將它指派給新方向最早仍無 reservation 的 frame。

舊工作實際完成順序可以亂序：

```text
106 先完成
102 後完成
```

但 reservation 的改派順序仍然必須是：

```text
93 → 92 → 91...
```

---

## 新方向的 SourceTexture GPU 傳輸也可以亂序

方向切換後，假設 reservation 已經是：

```text
96、97、98、99、100、101、102、103
```

其中某些 texture 內容已符合 reservation，某些仍需覆寫。

新方向的解碼與 GPU 傳輸可能依序完成：

```text
96 → 102 → 103 → 101
```

也可能：

```text
103 → 101 → 102 → 96
```

兩種都允許。

reservation 的作用是保證：

```text
SourceTexture reservation 已指派給 Frame 96
Frame 102 不能搶走 Frame 96 的 slot
```

但 reservation 不保證 Frame 96 一定先完成。

因此還需要 deadline 排程。

---

## 亂序執行與 deadline 排程

切換成反向後，若顯示順序為：

```text
99 → 98 → 97 → 96 → 95...
```

以 30 FPS 計算，各張 deadline 為：

| Frame | Deadline |
| ----: | -------: |
|    99 |  33.3 ms |
|    98 |  66.7 ms |
|    97 | 100.0 ms |
|    96 | 133.3 ms |
|    95 | 166.7 ms |

Frame 96 即使已經有 SourceTexture reservation，仍可能因讀檔、解碼或 GPU 傳輸太晚而錯過 133.3 ms 的 deadline。

所以各階段可以亂序執行，但排程優先級必須根據：

```text
距離顯示 deadline 的時間
```

較早要顯示的 frame 具有較高優先級。

例如 Frame 95 已經解碼完成，而 Frame 96 剛完成讀檔，可以先對 Frame 95 提交 GPU 傳輸，但不能讓大量較晚顯示 frame 的 GPU 傳輸長時間阻塞 Frame 96。

因此 GPU 傳輸 queue 也必須控制排隊深度，避免較晚顯示之 frame 的 GPU 傳輸占滿 queue。

---

## CompressedBuffer 階段的方向切換

假設 CompressedBuffer 有 8 個 slots，則永遠維持 8 個 CompressedBuffer reservation。

切換方向時，每個 reservation 分成三種情況。

### 尚未開始讀檔

可以立即改派給新方向 frame：

```text
改派：108 → 95
```

### 讀檔正在進行

不能立即把實體 buffer 當成空閒。

處理方式是：

```text
嘗試取消讀取
或
等待讀取返回
```

完成後：

* 若原 frame 再次成為需要的 frame，維持原指派
* 若不再需要，就將該 reservation 改派給新方向最早仍無 reservation 的 frame

### 已經讀取完成但尚未解碼

若壓縮內容正好是新方向需要的 frame，可以直接把 reservation 改派給該 frame，並沿用現有內容。

若不再需要，可以立即重新使用該 CompressedBuffer。

整個過程中：

```text
CompressedBuffer reservation 數量
=
CompressedBuffer slot 數量
```

不會因方向切換而額外增加另一批 reservation。

---

## UploadBuffer 階段的方向切換

UploadBuffer 同樣維持固定數量 reservation。

切換時可能有四種狀態。

### 尚未開始解碼

可以立即將 reservation 改派給新方向 frame。

### 解碼進行中

不能立即重用該 UploadBuffer。

必須等待目前的解碼完成，完成後若舊 frame 不再需要：

```text
不提交 GPU 傳輸
直接將 reservation 改派給新方向 frame
```

### 解碼完成，但 GPU 傳輸尚未提交

如果舊 SourceTexture reservation 已改派給其他 frame：

```text
不提交 GPU 傳輸
立即重新使用 UploadBuffer
```

如果已解碼的 frame 剛好仍在新方向需求內，則可沿用解碼結果，等待 SourceTexture reservation 指派給該 frame。

### GPU 傳輸已提交

UploadBuffer 必須保持有效，直到 GPU 傳輸完成。

GPU 傳輸完成後：

```text
將 reservation 改派給新方向最早仍無 reservation 的 frame
```

即使 GPU 傳輸的完成順序亂序，也不影響 reservation 的有序指派。

---

## 完整方向切換流程

假設播放到 Frame 100，從正向切換成反向。

新的顯示順序為：

```text
99 → 98 → 97 → 96 → 95...
```

整個 pipeline 執行以下步驟。

### 第一步：設定下一張顯示 frame

```text
nextFrameToDisplay = 99
```

畫面繪製與畫面提交不需要等待整條 pipeline 重建。

只要 Frame 99 的 SourceTexture 為 `Readable`，就能在下一個 display tick 顯示。

---

### 第二步：重新計算各階段需要的 frame 順序

SourceTexture 需要優先保證：

```text
99、98、97、96、95...
```

CompressedBuffer 與 UploadBuffer 的 reservation，也依相同的顯示 deadline 順序指派。

---

### 第三步：維持仍然有效的 reservation

方向切換前後都需要的 frame，不做任何改動。

例如：

```text
97、98、99、100、101、102、103
```

仍然在新的 SourceTexture reservation 範圍內，就維持原指派與實體內容。

---

### 第四步：立即改派尚未開始工作的 reservation

尚未讀檔、尚未解碼或尚未開始 GPU 傳輸的舊方向 reservation，可以立即改派給新方向 frame。

---

### 第五步：讓已開始的操作安全完成

正在讀檔、解碼、GPU 傳輸或畫面繪製的 slot，在操作完成前不改派 reservation。

每當一份舊方向 reservation 可以安全改派，就立即指派給新方向最早仍無 reservation 的 frame。

不必等待所有舊方向工作一起完成。

---

### 第六步：允許新方向工作亂序完成

新方向的讀檔、解碼與 GPU 傳輸可以依前一階段的實際完成順序執行。

但所需 frame 都已獲指派 reservation，因此較晚顯示的 frame 不會搶走較早顯示的 frame 容量。

---

### 第七步：畫面提交嚴格依新方向執行

即使 Frame 95 的 SourceTexture 比 Frame 96 更早進入 `Readable`，仍然只能：

```text
99 → 98 → 97 → 96 → 95
```

不能越過 SourceTexture 尚未進入 `Readable` 的 Frame 96。

---

## 如何維持 30 FPS

要保證方向切換後不中斷，需要同時滿足延遲與吞吐量條件。

### 準備足夠的逆向 frame

假設方向切換後，從重新排程到第一張需要重新準備的逆向 frame，其 SourceTexture 進入 `Readable` 的 p99 延遲為 `L` 秒。

逆向 frame 的數量至少為：

```text
B ≥ ceil(30 × L)
```

張剛顯示過的 SourceTexture reservation。

例如 p99 延遲為 200 ms：

```text
30 × 0.2 = 6
```

至少需要 6 張逆向 frame，實際還應加入安全餘量。

這些逆向 frame 都有正式的 SourceTexture reservation，不會增加額外 slot。

---

### 長期吞吐量必須高於 30 張／秒

方向切換只影響 pipeline 的工作方向，不會降低基本吞吐量要求。

系統仍需穩定達到：

```text
讀檔吞吐量 ≥ 30 張／秒
解碼吞吐量 ≥ 30 張／秒
GPU 傳輸吞吐量 ≥ 30 張／秒
```

以原始規格估算：

```text
PNG 讀取：
45 MiB × 30
≈ 1.32 GiB/s

GPU 傳輸：
126.56 MiB × 30
≈ 3.71 GiB/s
```

實際硬體必須有額外餘裕，不能只剛好等於理論值。

---

### 必須看連續具有 Readable SourceTexture 的 frame 數量

假設切換後各 frame 的 SourceTexture 狀態為：

```text
99 Readable
98 Readable
97 Readable
96 非 Readable
95 Readable
94 Readable
```

共有 5 個 frame 的 SourceTexture 為 `Readable`，但從 Frame 99 開始連續符合的只有：

```text
99、98、97
```

所以有效安全時間只有：

```text
3 × 33.33 ms ≈ 100 ms
```

Frame 95、94 提前完成，不能補償 Frame 96 的缺口。

因此監控指標應是：

```text
從 nextFrameToDisplay 開始，連續具有 Readable SourceTexture 的 frame 數量
```

而不是所有 `Readable` SourceTexture 的總數。

---

## 反覆切換 100、99、100、99

若 SourceTexture reservation 中同時存在：

```text
99
100
```

而兩張的 SourceTexture 都是 `Readable`，則：

```text
100 → 99 → 100 → 99 → 100 → 99
```

只是在兩個 SourceTexture 間重複畫面繪製與提交。

這段期間不需要：

```text
重新讀檔
重新解碼
重新執行 GPU 傳輸
```

顯示需求可以重複，但 frame 資源以 frame ID 重用。

其餘 SourceTexture reservation 仍可指派給兩側的順向與逆向 frame，例如：

```text
96、97、98、99、100、101、102、103
```

當播放位置真正向某一側持續移動時，再逐張移動 reservation 範圍。

---

## 為什麼這個方法有效

### reservation 數量永遠等於 slot 數量

不會因方向切換額外建立另一組 reservation，因此不會重複計算容量。

方向切換只是逐份替換 reservation。

---

### 已經開始的工作不會被錯誤重用

正在讀檔、解碼、GPU 傳輸或畫面繪製的實體資源，在操作完成前仍保持原 reservation。

因此不會發生同一個 slot 同時被兩個 frame 使用。

---

### 較早 frame 的容量受到保證

新方向 reservation 依顯示順序指派：

```text
99 → 98 → 97 → 96...
```

即使 Frame 95 比 Frame 96 更早完成，Frame 95 也不能搶走已指派給 Frame 96 的 SourceTexture 容量。

---

### 允許所有中間階段保持亂序

讀檔、解碼與 GPU 傳輸不需要等待前一張完成。

只要每張 frame 已有 reservation，就可以在前一階段完成時立即執行。

因此能維持平行 pipeline 的吞吐量。

---

### 能利用 SourceTexture 尚未被覆寫的內容

若某個 texture：

```text
目前內容 = 96
改派前的 reservedFrame = 103
GPU 尚未開始覆寫
```

方向切換後恰好需要 Frame 96，就可以將 reservation 改成 96，並將 slotState 直接改為 `Readable`。

不需要重新執行 GPU 傳輸。

---

### 能正確處理亂序 GPU 傳輸

舊方向與新方向的 GPU 傳輸都可以亂序完成。

系統只要求：

```text
reservation 的指派順序有序
實際工作的完成順序可以亂序
畫面提交順序嚴格有序
```

---

## 設計原則總結

整體設計可歸納為：

```text
1. 每個資源階段有固定數量的實體 slots。

2. CompressedBuffer、UploadBuffer 與 SourceTexture
   各自維護獨立的 reservation 表。

3. 每個階段的 reservation 數量永遠等於 slot 數量；
   reservation 代表容量保證，不是額外資源。

4. 同一個 frame 可以同時持有不同階段的 reservation；
   下游容量不足時，資料留在目前階段並占用原 slot。

5. reservation 依顯示需求順序指派，
   但集合不必連續，工作與 slot 重用都可以亂序完成。

6. decoder worker 是執行單元，不持有 reservation；
   slot 數量必須同時符合平行度與總記憶體容量限制。

7. 方向切換時，不建立第二組 reservation；
   只逐份改派原有 reservation。

8. 尚未開始工作的舊方向 reservation 可以立即改派。

9. 已開始的讀檔、解碼、GPU 傳輸或畫面繪製，
   必須等目前操作安全完成後才可改派 reservation。

10. 舊方向工作可以亂序完成；
   每當一份 reservation 可安全改派，就依新方向顯示順序指派給下一張 frame。

11. SourceTexture slot 必須同時記錄：
   reservedFrame、contentFrame 與 slotState。

12. 若 slotState 為 `Writable`，且 contentFrame 符合 reservedFrame，
   可以直接轉為 `Readable`，不需要重新執行 GPU 傳輸。

13. 新方向讀檔、解碼與 GPU 傳輸可以亂序完成。

14. SourceTexture 容量不足時，阻塞會經由 UploadBuffer
    與 CompressedBuffer 向上游傳遞，限制未完成工作總量。

15. reservation 保證容量，deadline 排程決定優先級；
    延遲、吞吐量與記憶體容量必須共同滿足 30 FPS 要求。

16. 畫面提交必須嚴格依目前播放方向執行。

17. 方向切換能力來自 SourceTexture reservation 範圍中的逆向 frame，
    不是依賴偶然尚未被覆寫的舊內容。

18. 30 FPS 的安全量應看 nextFrameToDisplay 起算，
    連續具有 `Readable` SourceTexture 的 frame 數量，
    不能只看所有 `Readable` SourceTexture 的總數。
```

整個模型可以概括為：

> **固定容量、固定數量 reservation、方向切換時逐份改派、工作亂序完成、結果依播放順序提交。**

這個方法既不會因方向切換重複計算容量，也能容納解碼與 GPU 傳輸的亂序行為，同時維持最終顯示順序與 30 FPS 的 deadline 要求。

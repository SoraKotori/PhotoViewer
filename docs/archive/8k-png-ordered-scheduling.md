# 8K PNG 有序播放的資源排程設計

> 此文件保存早期資源排程思路，不再定義目前執行角色或執行緒模型。現行設計以 [runtime-design.md](../runtime-design.md) 為準。

## 一、背景

系統需要連續讀取 PNG 圖片，經過解碼、GPU 傳輸與畫面顯示，目標規格如下：

* 影像尺寸：8K UHD，7680 × 4320
* 播放速度：每秒 30 張
* PNG 壓縮檔：約 45 MiB／張
* 解碼後格式：32-bit BGRA，約 126.56 MiB／張
* 多張圖片可同時解碼
* 最終顯示順序必須與 frame ID 完全一致

整體資料流程是：

```text
PNG 檔案
    ↓ 檔案讀取
CompressedBuffer
    ↓ 解碼
UploadBuffer
    ↓ GPU 傳輸
GPU Texture
    ↓ 畫面繪製
BackBuffer
    ↓ 畫面提交
顯示器
```

讀檔、解碼及 GPU 傳輸都可能亂序完成。例如 Frame 5 可能比 Frame 1 更早讀完或解碼完成，但最後仍必須按照：

```text
Frame 1 → Frame 2 → Frame 3 → ...
```

依序顯示。

---

## 二、核心問題

若完全禁止亂序，後面的 frame 必須等待前面的 frame，12 路平行解碼就難以充分利用。

但如果只要某張 frame 先完成，就讓它立即占用下一階段所有可用資源，也會造成另一個問題。

例如已發出 Frame 1～8 的讀檔：

```text
Frame 5、6、7、8 先讀完
Frame 1、2、3、4 尚未讀完
```

假設 5～8 使用了大部分 UploadBuffer，只替 Frame 1 留一個空位，那麼 Frame 1～4 隨後完成時，可能只能逐張進入解碼：

```text
Frame 1 進入
完成後 Frame 2 才能進入
再完成後 Frame 3 才能進入
```

原本的平行 pipeline 會退化成接近序列處理。

因此系統必須同時滿足兩個目標：

1. 允許 frame 亂序執行，以維持吞吐量。
2. 保證較早的 frame 不會被較晚的 frame 搶走所需容量。

---

## 三、解決方法：有序指派 reservation、亂序執行

每個資源階段都分成兩個概念：

### 1. 邏輯 reservation

reservation 表示：

> 這張 frame 已被保證可以使用該階段的一個 slot。

reservation 只代表容量保證，不代表已經占用某一個具體的實體 buffer。

### 2. 實體 slot

實體 slot 是真正的：

* CompressedBuffer
* UploadBuffer
* GPU Texture

只有當 frame 完成上一階段、真正準備開始工作時，才取得實體 slot。

---

## 四、reservation 的基本規則

假設某個階段共有 8 個 slots。

初始 reservation 必須按照 frame ID 順序指派：

```text
Frame 1、2、3、4、5、6、7、8
```

不能先指派給 Frame 10，而 Frame 3 還沒有 reservation。

若 Frame 5～8 先完成上一階段，它們可以立即取得四個實體 slots 並開始工作：

```text
Frame 5～8：正在使用四個實體 slots
Frame 1～4：持有 reservation，尚未使用實體 slots
```

剩餘四個實體 slots 必須保持可用，因此 Frame 1～4 即使同時完成，也能同時進入下一階段。

這避免了前序 frame 被序列化。

---

## 五、為什麼 reservation 不應預先綁定實體 slot

不需要預先指定：

```text
slot 0 永遠給 Frame 1
slot 1 永遠給 Frame 2
```

reservation 只保證總容量。

若有 8 個 slots，目前：

* 4 張 frame 正在使用 slot
* 4 張 frame 持有 reservation，但尚未完成上一階段

則一定還有 4 個空 slot，可以供這四張等待中的 frame 使用。

關係可以表示為：

```text
正在使用的 frame
+
已指派 reservation 但尚未使用 slot 的 frame
≤
實體 slot 總數
```

因此，reservation 已指派給某個 frame 後，該 frame 完成上一階段時，不會因後面的 frame 而找不到資源。

---

## 六、slot 可重用後如何繼續前進

Frame 5～8 若先完成該階段，其 slots 可以安全重用。

這些 reservation 再依 frame ID 順序改派給：

```text
Frame 9、10、11、12
```

此時持有 reservation 的 frame 集合可能是：

```text
Frame 1、2、3、4、9、10、11、12
```

這個集合不再是連續區段，但仍然正確：

* Frame 1～4 的容量仍受到保證。
* Frame 5～8 不再使用的資源不會閒置。
* Frame 9～12 可以繼續推進 pipeline。

因此正確原則不是「整個視窗必須等最早 frame 完成才一起前移」，而是：

> reservation 依 frame ID 有序指派，但已完成 frame 的資源可以依實際完成順序重用。

---

## 七、不同資源階段的行為

### CompressedBuffer

CompressedBuffer 保存壓縮 PNG。

生命週期：

```text
開始讀檔
→ 讀檔完成
→ 等待解碼
→ 解碼
→ 解碼完成後重用
```

Frame 解碼完成後，該壓縮資料通常已不再需要，因此 CompressedBuffer 可以按照解碼完成順序亂序重用。

若有 16 個 CompressedBuffer，每張約 45 MiB：

```text
最大壓縮資料空間約為：

16 × 45 MiB = 720 MiB
```

實際數量應同時受 slot 數量與總記憶體容量控制。

---

### UploadBuffer

UploadBuffer 保存解碼輸出，同時也是 GPU 傳輸的來源。

生命週期：

```text
開始解碼寫入
→ 解碼完成
→ 等待 GPU 傳輸
→ GPU 傳輸
→ 傳輸完成後重用
```

UploadBuffer 可以依 GPU 傳輸的完成順序亂序重用。

如果較晚的 frame 先完成 GPU 傳輸，其 UploadBuffer 不必等待較早 frame，即可重新供後續 frame 使用。

UploadBuffer 的數量需要涵蓋同時解碼與等待 GPU 傳輸的 frame。

---

### GPU Texture

GPU Texture 位於 GPU，保存已完成傳輸的 frame，供畫面繪製使用。

生命週期：

```text
GPU 傳輸
→ 等待顯示順序
→ 畫面繪製
→ GPU 不再使用後重用
```

GPU Texture 與前兩種資源不同。

即使 Frame 5 已經完成 GPU 傳輸，只要 Frame 1～4 尚未顯示，Frame 5 就不能先顯示，因此該 GPU Texture 不能重用。

GPU Texture 的重用順序通常接近顯示順序，這也是整個 pipeline 最容易形成阻塞的位置。

---

## 八、解碼執行容量與 slot 的差別

解碼執行容量負責處理資料，不負責長期保存 frame；執行角色與工作生命週期統一由 [runtime-design.md](../runtime-design.md) 定義。

真正需要 reservation 的，是會跨越非同步階段並長時間被持有的：

* CompressedBuffer
* UploadBuffer
* GPU Texture

---

## 九、最終顯示仍必須完全有序

前面的讀檔、解碼及 GPU 傳輸都可以亂序執行。

但畫面繪製與畫面提交只能接受下一張應顯示的 frame：

```text
nextFrameToDisplay = Frame 100
```

即使 Frame 101、102 已經在 GPU 上，也必須等待 Frame 100。

因此整體模型是：

```text
有序指派 reservation
        ↓
亂序讀取
        ↓
亂序解碼
        ↓
亂序 GPU 傳輸
        ↓
有序畫面繪製與提交
```

---

## 十、為什麼這個方法有效

這個設計同時解決了順序與性能問題。

### 保證較早 frame 的性能

reservation 已經指派給較早 frame，因此較晚 frame 不會占用掉它所需要的容量。

即使 Frame 5～8 先完成，Frame 1～4 仍能在完成上一階段時同時進入下一階段，不會退化成逐張執行。

### 充分利用可重用的資源

Frame 5～8 完成後，其 slots 可以立即重用，再依序提供給 Frame 9～12，不必等待 Frame 1 完成。

這能維持 pipeline 的吞吐量。

### 維持最終顯示順序

即使中間階段完全亂序，最後仍按照 frame ID 繪製與提交，因此畫面順序不會錯亂。

---

## 十一、設計原則總結

整個系統應遵守以下原則：

```text
1. 每個資源階段有固定數量的實體 slots。

2. reservation 依 frame ID 順序指派。

3. reservation 只保證容量，不預先綁定實體 slot。

4. reservation 已指派且完成上一階段的 frame，可以立即取得實體 slot。

5. 讀檔、解碼及 GPU 傳輸可以亂序執行與完成。

6. CompressedBuffer 與 UploadBuffer 可以依完成順序亂序重用。

7. GPU Texture 受有序顯示限制，通常較接近顯示順序重用。

8. 畫面繪製與畫面提交必須嚴格依 frame ID 執行。
```

這個模型可以概括為：

> **有序指派容量，亂序使用資源，有序提交結果。**

它讓較早 frame 的資源需求受到保證，同時讓較晚且較快完成的 frame 能立即使用可重用資源，適合高吞吐量、亂序完成但最終必須有序顯示的影像 pipeline。

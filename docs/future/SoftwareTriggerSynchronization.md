# Software Trigger同期の将来改善

## 状態

`feature/hw-sw-sync`の実機確認では、HW triggerは安定して動作し、`HostReceived`によるframe対応付けを4 msまで狭めて動作確認できました。

一方、現在のSW trigger実装は、複数カメラへ`TriggerSoftware` commandを順番に送信するため、同期精度と安定性が十分ではありません。

```text
HW trigger : 実機確認済み・推奨
SW trigger : 実験的・将来改善対象
```

SW trigger APIとサンプル引数は削除せず、将来の改善と比較評価のため残します。

## 現在の実装

現在の解析サンプルは、1回の取得要求について各カメラへ順番にcommandを送ります。

```text
camera 0 -> TriggerSoftware
camera 1 -> TriggerSoftware
camera 2 -> TriggerSoftware
...
```

この方式では、次の時間がカメラ間の差として加算されます。

```text
API呼出し時間
GenTL処理時間
USB control transfer待ち
OS thread scheduling
camera側command受付時間
```

そのため、同じループ内でcommandを送っても同時露光にはなりません。

## 将来の調査項目

### 1. command発行経路の計測

各カメラについて次を記録します。

```text
command送信直前のhost time
command API完了直後のhost time
最初に返ったframeのdevice timestamp
最初に返ったframeのhostReceivedTime
frame number
```

これにより、command送信時間、commandからframe受信までの遅延、カメラ間jitterを分離して評価します。

### 2. cameraごとの遅延補正

カメラごとにSW trigger commandから露光・frame到着までの固定offsetが観測できる場合、次を検討します。

```text
camera別のcommand先行時間
camera別のtimestamp補正値
起動後の短時間calibration
一定期間ごとの補正値再推定
```

固定offsetではなくjitterが支配的な場合、この方式だけでは改善できません。

### 3. command発行専用scheduler

表示・readback・OpenCV解析を行うthreadからtrigger発行を分離します。

```text
high-priority trigger scheduler thread
  -> camera 0 command
  -> camera 1 command
  -> target periodまでwait
```

検討項目:

```text
専用thread priority
CPU affinity
steady_clockによるabsolute deadline
sleepとspin waitの切替
command発行順の交互化
```

ただし、USB control transfer自体を同時化できるとは限らないため、実測で効果を判断します。

### 4. 並列command発行

カメラごとに専用threadを用意し、barrier解除後に同時に`TriggerSoftware`を実行する方式を検証します。

```text
main scheduler
  -> barrier release
      -> camera 0 trigger thread
      -> camera 1 trigger thread
```

確認事項:

```text
IC4 Grabberのthread safety
同一USB host controller上のcontrol transfer直列化
thread wake-up jitter
発行順の再現性
```

### 5. device timestampの利用可能性

SW trigger後のframeについて、device timestampの原点が異なっていても、起動時offsetを推定して差分比較できるか検証します。

```text
normalizedDeviceTs = deviceTimestampNs - estimatedCameraEpochOffset
```

clock driftが大きい場合は、継続的なoffset更新が必要です。

### 6. USB構成の影響

次を比較します。

```text
2台を同一USB host controllerへ接続
2台を別USB host controllerへ接続
USB hubあり / なし
camera順序を入替
```

SW triggerの不安定性がcommand実装ではなくUSB control transfer競合による可能性も分離します。

## 評価方法

HW triggerを基準条件として、同じカメラ設定と被写体で比較します。

```text
Camera state       : gamma1
Resolution         : 1536 x 1536
PixelFormat        : BayerRG8
OffsetX            : 236
OffsetY            : 0
Frame matching     : TimestampNearest
Timestamp source   : HostReceived
```

記録指標:

```text
trigger回数
各cameraの取得frame数
sync emitted set数
sync dropped frame数
hostReceivedTime差の平均・標準偏差・最大値
command発行時刻差
read error数
連続動作中の脱落区間
```

## 完了条件

SW triggerを「実機利用可能」とするには、少なくとも次を満たすこととします。

```text
10分以上の連続動作
全cameraのread errorが0
trigger回数と取得set数の差が説明可能な範囲
hostReceivedTime差の分布が安定
同期許容差を固定した状態でdropが継続的に増加しない
同じ条件で複数回再現する
```

HW triggerと同等の精度をSW triggerへ要求するかは、用途ごとに別途定義します。

## 当面の運用

同期精度が必要な用途ではHW triggerを使用します。

```text
--trigger-mode hardware
--trigger-source Line1
--max-timestamp-diff-ns 4000000
```

SW triggerは実験・計測目的に限定します。

# pmtel：读出“CPU 封装为什么停在 C2”（只读）

macOS 自带的 `com.apple.driver.pmtelemetry` 会通过 IOReport 公布 CPU 和芯片组自己记录的封装 C-state 计数器。你的 ioreg 里已经能看到这些通道：

- 各级封装 C-state 进入次数：`PKGC_C2/C3/C6/C7…_ENTRANCE_COUNTER`
- 封装进不了更深状态的原因：`PKGC_BREAK_CAUSE_*`（例如 `NOT_L1`、`LATENCY`、`PCH_PMREQ`）、`PKGC_BCLK_RESOLVE_CAUSE_*`（例如 `PEGS_ENABLED`、`PCH_NACK`）
- 每个芯片组设备当前上报的延迟容忍值（LTR）：`SPA/SPB/SPC/SPD/SPE`（PCIe 根端口组）、`SATA`、`XHCI`、`HDA`、`GBE`、`ME`、`PEG0-2` 等
- 芯片组各部分的活动计数：PLL、PCIe/USB PHY 通道、SATA、USB 等

pmtel 在你不动电脑的 20 秒里采样两次，把差值打印出来。

**它只读。** 不加载内核扩展，不改任何设置、NVRAM 或文件，退出后不留下任何东西。

## 编译（GitHub Actions）

1. 新建一个仓库，或者用 InitHeadphone 那个仓库也可以。
2. 在仓库根目录添加 `pmtel.c`。
3. 在仓库里新建文件，路径填 `.github/workflows/pmtel.yml`，内容用本目录的 `pmtel.yml`。
4. 打开 Actions → `pmtel build`，它会自动运行（也可以点 Run workflow）。完成后下载产物 `pmtel`。

如果编译失败，请下载 `pmtel-build-log` 发我。

## 运行（约 25 秒）

保持平时的状态：屏幕亮着，接不接电源都可以，但不要放视频或音乐。

```bash
cd ~/Downloads
[ -f pmtel.zip ] && unzip -o pmtel.zip
tar -xzf pmtel.tar.gz && xattr -c pmtel
sudo ./pmtel 20 > ~/Desktop/pmtel.txt; pmset -g batt | head -n 2 >> ~/Desktop/pmtel.txt
```

运行期间不要碰键盘和触控板。把桌面上的 `pmtel.txt` 发我。

## 说明

- `pmset -g batt` 只读取电源状态（交流还是电池），不改设置。
- 输出里 `delta` 是 20 秒内的增量，`now` 是当前值。LTR 的 `Raw` 值会按 Intel PCH 的寄存器格式解码成微秒。
- libIOReport 是 macOS 的私有库。pmtel 在运行时按名字查找函数，所以编译不依赖 SDK 里的存根库。

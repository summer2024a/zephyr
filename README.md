
## menuconfig
```
west build -t menuconfig -d build_a53_shell_fs
```

## Zephyr 在 QEMU A53 平台运行 Shell 控制台实例 

```
west build -b qemu_cortex_a53 zephyr/samples/subsys/shell/shell_module -d build_qemu_a53
qemu-system-aarch64 \
  -global virtio-mmio.force-legacy=false \
  -cpu cortex-a53 \
  -machine virt,secure=on,gic-version=3 \
  -net none \
  -pidfile qemu.pid \
  -chardev stdio,id=con,mux=on \
  -serial chardev:con \
  -mon chardev=con,mode=readline \
  -nographic \
  -icount shift=4,align=off,sleep=on \
  -rtc clock=vm \
  -kernel /work/zephyrproject/build_qemu_a53/zephyr/zephyr.elf
```

## Zephyr 在 QEMU A53 平台运行 Shell 控制台实例 + 文件系统

```
编译
west build -b qemu_cortex_a53 app_shell_fs -d build_a53_shell_fs

宿主机启动virtiofsd
mkdir -p /tmp/virtiofs-share
/usr/lib/qemu/virtiofsd --socket-path=/tmp/vhostqemu -o source=/tmp/virtiofs-share

qemu-system-aarch64 \
  -global virtio-mmio.force-legacy=false \
  -cpu cortex-a53 \
  -machine virt,secure=on,gic-version=3 \
  -net none \
  -pidfile qemu.pid \
  -chardev stdio,id=con,mux=on \
  -serial chardev:con \
  -mon chardev=con,mode=readline \
  -nographic \
  -icount shift=4,align=off,sleep=on \
  -rtc clock=vm \
  -chardev socket,id=char0,path=/tmp/vhostqemu \
  -device vhost-user-fs-device,queue-size=1024,chardev=char0,tag=myfs \
  -m 32M \
  -kernel ~/xia/zephyrproject/build_a53_shell_fs/zephyr/zephyr.elf
```

## 编译 He200 板卡（Shell + 8 核 SMP）

移植说明、SPL 直连启动、多核与外设路线图见 [zephyr/boards/lynxi/PORTING.md](zephyr/boards/lynxi/PORTING.md)。

推荐板型 **`he200_ep`**（链接地址 `0x800100000`，与 SPL 烧录 `u-boot.bin` 一致）：

```bash
# 在 zephyrproject 工作区根目录执行
west build -b he200_ep -d build_he200_ep_final app_shell_fs --pristine
# 产物：build_he200_ep_final/zephyr/zephyr.bin
```

可选：`west build -t menuconfig -d build_he200_ep_final` 调整 Kconfig。  
通用板 `he200`：`west build -b he200 -d build_he200 app_shell_fs --pristine`

### 烧录

将 `zephyr.bin` 写入原 **u-boot.bin** 分区（与 RT-Thread 相同 SPL 路径）。

### 串口与 Shell

- 115200，UART0，时钟 50MHz
- 命令：`he200_smp`、`kernel thread stacks`（验收 8 核见 PORTING.md §10.1）

### 8 核 SMP（默认已开启）

启动应出现 `Secondary CPU core 1..7 (MPID:…)` 与 `he200_ep: SMP=on online_cpus=8`。详见 PORTING.md。

---

## He200 板卡入口点偏移量分析

### 问题背景
在使用 U-Boot 启动 Zephyr 固件时，发现入口点 `__reset` 位于 `0x800105aec`，而不是加载地址 `0x800100000`。这个偏移量引发了对内存布局的疑问。

### 详细分析结果

#### 内存布局（改进后，zephyr.map）
```
0x800100000
├── .image_header            (64 bytes, 0x40)
├── .text._reset_section     (304 bytes, 0x130)
│   ├── __reset_prep_c       (0x800100040)
│   ├── __reset              (0x8001000c4 - 入口点)
│   └── __start
├── .exc_vector_table        (2048 bytes, 0x800)
├── _vector_table            (1924 bytes, 0x784)
├── 填充对齐                 (12 bytes)
├── .text（其他函数）        (~22KB)
│   ├── z_prep_c
│   ├── z_arm64_el3_init
│   ├── z_arm64_el1_init
│   └── ... 其他初始化函数
```

#### 偏移量计算
```
偏移量分析（从 0x800100000 开始）：
image_header: 0x000000 (size: 0x40)
_reset_section: 0x000040 (size: 0x130)
reset_entry: 0x0000c4  <-- 新的入口点偏移量
vector_start: 0x000170
vector_table: 0x000800 (size: 0x784)
vector_end: 0x000f84
text_start: 0x000f90
```

### Image Header 智能跳转设计分析

#### ARM64 Image Header 工作原理

ARM64 image_header 采用了**隐式入口点设计**，使用直接分支指令而非专门的字段来表示入口地址：

```asm
b	__start				// branch to kernel start
```

#### 分支指令智能跳转验证

```python
# 分析优化前固件的分支指令
分支指令: 0x140016bb
立即数: 0x0016bb
跳转偏移: 0x00005aec
绝对地址: 0x0000000800105aec
与入口点的差异: 0x00000000
```

**验证结果：** 分支指令已经正确跳转到入口点！

#### 内存布局分析

**原始布局：**
```
0x800100000
├── .image_header            (64 bytes, 0x40)
├── .exc_vector_table        (2048 bytes, 0x800)
├── _vector_table            (1924 bytes, 0x784)
├── 填充对齐                 (12 bytes)
├── .text（启动相关函数）    (~22KB)
│   ├── __reset_prep_c       (0x800105a68)
│   ├── __reset              (0x800105aec - 入口点)
│   ├── z_prep_c
│   ├── z_arm64_el3_init
│   ├── z_arm64_el1_init
│   └── ... 其他初始化函数
```

#### 为什么能正常工作？

**智能跳转机制：**
- **偏移量预计算**：在编译过程中，链接器会计算 `__start` 函数相对于 image_header 的偏移量
- **动态跳转地址计算**：分支指令格式自动计算跳转地址
- **兼容性保证**：无论代码如何分布，只要 image_header 和程序代码匹配，跳转就会正确执行

**执行流程：**
1. U-Boot 加载固件到 `0x800100000`
2. 执行 `go 0x800100000` 会使 CPU 开始执行 image_header
3. 第一条指令会无条件跳转到 `0x800105aec` 的入口点

#### 验证方法

```bash
# 编译优化前固件
west build -b he200_ep -d build_he200_ep_final app_shell_fs --pristine

# 检查入口点
/opt/toolchains/zephyr-sdk-1.0.0/gnu/aarch64-zephyr-elf/bin/aarch64-zephyr-elf-readelf -h build_he200_ep_final/zephyr/zephyr.elf
```
1. **image_header 和 exc_vector_table 只占前 4KB**
2. **大量架构特定的初始化代码**占用了剩余空间：
   - 中断处理向量
   - 内存管理初始化
   - 线程管理初始化
   - 异常处理函数
   - 系统时钟初始化
   - 其他启动代码

**不是简单的两个段直接占据了 0x5aec，而是整个启动代码段的集合导致了这个偏移量。**

### 验证方法

```bash
# 查看ELF文件信息
/opt/toolchains/zephyr-sdk-1.0.0/gnu/aarch64-zephyr-elf/bin/aarch64-zephyr-elf-readelf -h zephyr.elf

# 查看段信息
/opt/toolchains/zephyr-sdk-1.0.0/gnu/aarch64-zephyr-elf/bin/aarch64-zephyr-elf-readelf -S zephyr.elf

# 查看详细内存布局
grep -A 20 -B 5 "image_header\|exc_vector_table\|vectors\|__reset" zephyr.map
```
```
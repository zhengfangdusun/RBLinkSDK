# 克隆参考例程 rbfclone {#clone_example}

仅使用公开的 `rbf_api.h`、`rbf_clone.h`，动态链接 SDK。与 `rbftest` 并列构建，演示文件读写回调、异步状态通知及应用线程调用 finish/cancel。

在 SDK 源码或 Linux 发布包根目录配置对应工具链。发布包首次构建先选择平台（mt76x8 示例）：

```sh
cp platform/linux/mips_mt76x8/CMakeLists.txt ./CMakeLists.txt
```

随后构建两个例程：

```sh
cmake -S . -B build
cmake --build build --target rbftest rbfclone
```

源码构建程序在 `build/test/`；发布包构建程序在 `build/`。运行时用 `LD_LIBRARY_PATH` 指向对应平台 `librbf.so` 所在目录。

## 同一 Hub 导出、清空、恢复

先确保串口未被其他程序占用。每步成功退出后再继续；任一步失败就停止，不执行后续清空/导入。

```sh
./rbfclone export /dev/ttyS1 backup.raw
sha256sum backup.raw > backup.raw.sha256
# 将 backup.raw 及摘要另存一份，再确认设备清单。
./rbfclone probe /dev/ttyS1
./rbfclone factory-reset /dev/ttyS1
./rbfclone probe /dev/ttyS1
sha256sum -c backup.raw.sha256
./rbfclone import /dev/ttyS1 backup.raw
./rbfclone probe /dev/ttyS1
```

导出保留设备且不覆盖已有文件，每片写盘/fsync后才确认；`factory-reset` 显式清空并重启，清单应变为0。导入以234字节分片读取原始文件，在DATA_READY后由主线程finish，确认正常退出后查询；最终清单应与原清单一致。失败/超时不等于远端已退出，不得自动重发恢复出厂。

例程不负责业务停启、备份摘要管理或RF/设备版本验收；原始备份可能含设备密钥，应妥善保管。默认不打印数据内容，`RBF_CLONE_TRACE=1` 仅用于受控诊断。恢复出厂超时的显式恢复接口用法见SDK克隆接口文档。

## 完整源码

\include rbfclone.c

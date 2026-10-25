# ESP-Hosted 本地补丁

`esp-hosted-local.patch` 保存了当前 Hosted 子模块的三处本地修改：

- `esp/esp_driver/network_adapter/main/cmd.c`
- `host/esp_cfg80211.c`
- `host/include/esp.h`

这些修改用于本板的 OpenWrt AP 配置路径。它们**不是 Espressif 官方提交**，不应直接推送到官方仓库。父仓库的子模块指针保持不变；当前子模块 dirty 工作树也不要清理。

## 在干净的子模块上应用

```sh
git -C external/esp-hosted-linux apply --check ../../docs/patches/esp-hosted-local.patch
git -C external/esp-hosted-linux apply ../../docs/patches/esp-hosted-local.patch
```

如果当前子模块已有本地改动，先保留 diff，不要执行 `git clean`、`checkout -- .` 或强制覆盖。父仓库仍引用原 Hosted 子模块版本；补丁只是本地可复现的附加修改，发布流程需要明确是否启用它。

# 真机验收(设备到货后)

烧录前在 `ai-passport/` 做一次 `idf.py fullclean && idf.py build`,让 `sdkconfig.defaults` 里关掉的 LVGL 示例 / NimBLE Central·Observer 生效。烧录步骤见 `dist/firmware/FLASH.md`(本地产物,不入库)。

## 清单

- [ ] USB 串口启动日志稳定,无重启循环/断言/看门狗复位
- [ ] 屏幕方向/颜色/背光正常;LISTENING 为麦克风图标,不是 REC 音量条
- [ ] UP/DOWN/OK:松手发送;DOWN=回车;OK 双击清空
- [ ] 音频:采集非零、播放正常、采样率正确
- [ ] BLE:Mac companion 发现 0xA2B0、连接、PTT 转写

## Companion(Mac)

- [ ] 悬浮窗无标题栏、无关闭/最小化/最大化
- [ ] 按住说话时桌面不转圈
- [ ] 松手后字进当前输入框,不是进悬浮窗
- [ ] 连说十几秒:开头有没有丢字(若有,把 `companion/relay.py` 的 `AUDIO_Q_MAX` 从 20 调到 30–40)
- [ ] 说话过程中设备不要跳到审批页(GUI 已 `do_approval=False`)

## 资源

- [ ] 电池读数合理;CW2017 缺席时降级安全
- [ ] 控制台 `st`:堆余量和任务栈 HWM;反复切页无持续泄漏

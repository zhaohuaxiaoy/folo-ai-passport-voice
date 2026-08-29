# 真机验收(设备到货后)

烧录前在 `ai-passport/` 做一次 `idf.py fullclean && idf.py build`,让 `sdkconfig.defaults` 里关掉的 LVGL 示例 / NimBLE Central·Observer 生效。烧录步骤见 `dist/firmware/FLASH.md`(本地产物,不入库)。

## 清单

- [ ] USB 串口启动日志稳定,无重启循环/断言/看门狗复位
- [ ] 屏幕方向/颜色/背光正常;LISTENING 为麦克风图标,不是 REC 音量条
- [ ] 两级息屏:20s 无键背光灭(面板冻结最后一帧);60s 面板 SLPIN 断电;
      任意键 PRESS 唤醒(背光 + 面板);息屏时 CLICK 不唤醒;APPROVAL 不熄屏
- [ ] 面板唤醒后内容恢复(SLPOUT 120ms 时序;DRAM 保留,应免重绘)
- [ ] UP/DOWN/OK:按住 UP 说话(按下即录,滴声后立即开始)松手发送;DOWN 单击=回车;DOWN 长按 0.5s=清空(一声确认音)
- [ ] 按下 UP 到滴声/开录的间隔 <200ms(串口 `采集开始` 与按键日志比对;不足 0.5s 的误碰不进转写)
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
- [ ] 省电门禁:任何操作后 `st` 的 `--- pm ---` 显示 `normal`,静默满 1 分钟转 `saving`
      (串口同时打一条「空闲 60s:进入省电」);再按任意键立刻回 `normal`。
      注意 USB 在位时固件主动禁 light sleep(否则串口失联),只有拔线跑电池才睡
- [ ] PTT 慢的自证:串口出现「滴声兜底开流」= 那一按走的是超时兜底(慢 200ms+),
      没有这行就是正常路径(实测 ~60ms)
- [ ] 控制台 `st`:堆余量和任务栈 HWM;反复切页无持续泄漏

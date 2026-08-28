# 定性 ring 计数失同步并加固排空诊断

## Goal

test_cancel_idempotent 3/15 偶发永久拒绝 start(启动被拒:上一次会话残留未排空)。代码审计已排除固件常态失同步路径(生产侧 send 成功才加计数;消费侧三路径全部归还递减),重点:①fake_rtos 建模差异 ②证据型加固(start 拒绝分支加环物理状态+HWM 日志;stop/cancel 记录 wait_worker_exit 超时)。

## Requirements

- TBD

## Acceptance Criteria

- [ ] TBD

## Notes

- Keep `prd.md` focused on requirements, constraints, and acceptance criteria.
- Lightweight tasks can remain PRD-only.
- For complex tasks, add `design.md` for technical design and `implement.md` for execution planning before `task.py start`.

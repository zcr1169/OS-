#!/usr/bin/env python3
"""自动化测试脚本 — 按测试流程.md 逐项验证"""
import subprocess, os, sys, time, re
sys.stdout.reconfigure(encoding='utf-8')

BASE = os.path.dirname(__file__)
EXE = os.path.join(BASE, '生成', 'OSCD.exe')
DATA = os.path.join(BASE, '生成', '数据')
ROOT_DATA = os.path.join(BASE, '数据')
passed = 0
failed = 0
errors = []

def clean():
    for d in [DATA, ROOT_DATA]:
        if os.path.exists(d):
            import shutil; shutil.rmtree(d)

def run(commands, timeout=15, keep_data=False):
    """运行程序并返回输出"""
    if not keep_data:
        clean()
    proc = subprocess.Popen(
        EXE, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, cwd=BASE, shell=False
    )
    out, _ = proc.communicate(input=commands.encode('utf-8'), timeout=timeout)
    return out.decode('utf-8', errors='replace')

def check(name, commands, *expected, neg=None):
    global passed, failed
    output = run(commands)
    for exp in expected:
        if exp in output:
            passed += 1
        else:
            failed += 1
            msg = f'[失败] {name}: 未找到 "{exp}"'
            errors.append(msg)
    if neg:
        for exp in neg:
            if exp in output:
                failed += 1
                msg = f'[失败] {name}: 不应包含 "{exp}"'
                errors.append(msg)
            else:
                passed += 1
    return output

def check_out(name, output, *expected, neg=None):
    global passed, failed
    for exp in expected:
        if exp in output:
            passed += 1
        else:
            failed += 1
            msg = f'[失败] {name}: 未找到 "{exp}"'
            errors.append(msg)
    if neg:
        for exp in neg:
            if exp in output:
                failed += 1
                msg = f'[失败] {name}: 不应包含 "{exp}"'
                errors.append(msg)
            else:
                passed += 1

print('=' * 60)
print('OS 模拟器 自动化测试')
print('=' * 60)

# ========== 一、用户管理 ==========
print('\n--- 一、用户管理 ---')

check('1.1 注册', 'register admin 123456\nexit\n', '注册成功')
check('1.2 重复注册', 'register admin 123456\nregister admin 123456\nexit\n', '注册成功', '注册失败')
check('1.3 登录', 'register admin 123456\nlogin admin 123456\nexit\n', '登录成功')
check('1.4 错误密码', 'register admin 123456\nlogin admin 111111\nexit\n', '登录失败')
check('1.5 未登录', 'register admin 123456\nlogin admin 123456\nlogout\ncreate_pcb test 5\nexit\n', '已登出', '请先登录')

# ========== 二、进程管理 ==========
print('\n--- 二、进程管理 ---')

check('2.1 创建', 'register admin 123456\nlogin admin 123456\ncreate_pcb pa 3\ncreate_pcb pb 7\ncreate_pcb pc 1\nexit\n', 'PID=2', 'PID=3', 'PID=4')

check('2.3 list_pcb', 'register admin 123456\nlogin admin 123456\ncreate_pcb pa 3\ncreate_pcb pb 7\ncreate_pcb pc 1\ncreate_pcb ca 5 2 3\ncreate_pcb cb 6 2 2\nlist_pcb\nexit\n', 'pa', 'pb', 'ca', 'cb')

check('2.4 show_pcb', 'register admin 123456\nlogin admin 123456\ncreate_pcb pa 3\nshow_pcb 2\nexit\n', 'PID', 'pa', '就绪', '优先级', 'CPU时间', '占用内存')
check('2.5 ptree', 'register admin 123456\nlogin admin 123456\ncreate_pcb pa 3\ncreate_pcb ca 5 2 3\nptree\nexit\n', '进程树', 'pa', 'ca')
check('2.6 block', 'register admin 123456\nlogin admin 123456\ncreate_pcb pa 3\nblock_pcb 2\nshow_pcb 2\nexit\n', '已阻塞', '阻塞')
check('2.7 wakeup', 'register admin 123456\nlogin admin 123456\ncreate_pcb pa 3\nblock_pcb 2\nwakeup_pcb 2\nshow_pcb 2\nexit\n', '已唤醒', '就绪')
check('2.8 suspend', 'register admin 123456\nlogin admin 123456\ncreate_pcb pa 3\nsuspend 2\nexit\n', '已挂起')
check('2.9 resume', 'register admin 123456\nlogin admin 123456\ncreate_pcb pa 3\nsuspend 2\nresume 2\nexit\n', '已挂起', '已恢复')
check('2.10 renice', 'register admin 123456\nlogin admin 123456\ncreate_pcb pa 3\nrenice 2 0\nshow_pcb 2\nexit\n', '优先级已修改', '优先级:    0')

# kill_pcb 测试: 只检查 list_pcb 部分不含被杀的进程
output = run('register admin 123456\nlogin admin 123456\ncreate_pcb pa 3\ncreate_pcb ca 5 2 3\nkill_pcb 2\nlist_pcb\nexit\n')
# 提取 list_pcb 之后的部分
idx = output.find('PID   名称')
list_output = output[idx:] if idx >= 0 else ''
check_out('2.11 kill_pcb 已撤销', output, '已撤销')
check_out('2.11 kill_pcb 列表无pa', list_output, neg=['pa', 'ca'])

# ========== 三、调度器 ==========
print('\n--- 三、调度器 ---')

out = check('3.1 创建CPU进程', 'register admin 123456\nlogin admin 123456\ncreate_pcb wa 2 1 10\ncreate_pcb wb 5 1 10\ncreate_pcb wc 10 1 10\nexit\n', 'PID=2', 'PID=3', 'PID=4')

# 单步调度
out = run('register admin 123456\nlogin admin 123456\ncreate_pcb sa 3 1 6\ncreate_pcb sb 8 1 6\nstep\nstep\nstep\nstep\nstep\nstep\nexit\n')
check_out('3.3 step 降级', out, '时间片耗尽, 降级')
check_out('3.3 step 完成', out, '进程完成! 已自动终止')
check_out('3.3 step 队列格式 [0/6]', out, '[0/6]')
check_out('3.3 step 无可调度', out, '无可调度进程')

check('3.4 start/stop', 'register admin 123456\nlogin admin 123456\ncreate_pcb wa 2 1 10\ncreate_pcb wb 5 1 10\ncreate_pcb wc 10 1 10\nstart_sched\nstop_sched\nexit\n', '调度器已启动', '调度器已停止', 'Q0', 'Q1', 'Q2')

# ========== 四、内存管理 ==========
print('\n--- 四、内存管理 ---')

out = run('register admin 123456\nlogin admin 123456\ncreate_pcb mt 5 1\nalloc 100 2\nalloc 200 2\nalloc 50 2\nfree_mem 100\nshow_mem\ncompact\nmem_stat\nexit\n')
check_out('4.x show_mem', out, '空闲', '已分配', '内存分配图', '内存块总览', '碎片率')
check('4.7 set_alloc_algo', 'register admin 123456\nlogin admin 123456\nset_alloc_algo BF\nset_alloc_algo WF\nset_alloc_algo FF\nexit\n', '最佳适应', '最坏适应', '首次适应')
check('4.8 pgfault', 'register admin 123456\nlogin admin 123456\npgfault\nexit\n', '缺页中断', '页面错误', '页表')

# alloc 非进程目标
out = run('register admin 123456\nlogin admin 123456\nalloc 64 data\nalloc 32 io\nalloc 16 kernel\nshow_mem\nexit\n')
check_out('alloc data', out, '数据', 'IO', '内核')
check('alloc data 成功消息', 'register admin 123456\nlogin admin 123456\nalloc 64 data\nexit\n', '目标=数据')

# swap_out + swap_in
out = run('register admin 123456\nlogin admin 123456\ncreate_pcb mt 5 1\nalloc 100 2\nswap_out 2\nswap_in 2\nexit\n')
check_out('swap_out', out, '换出操作', '标记为')
check_out('swap_in', out, '换入操作', '重新分配')

# ========== 五、持久化 ==========
print('\n--- 五、持久化 ---')

# 先 save，不清理数据
out1 = run('register admin 123456\nlogin admin 123456\ncreate_pcb sa 3 1 4\ncreate_pcb sb 5 1 4\nalloc 128 2\nalloc 256 2\nsave\nexit\n', keep_data=False)
check_out('5.1 save', out1, 'os_state.bin')

# 第二次启动，保留数据文件验证 load
out2 = run('login admin 123456\nlist_pcb\nshow_mem\nexit\n', keep_data=True)
check_out('5.2 load 进程', out2, 'sa', 'sb')
check_out('5.2 load 内存', out2, 'PID2')

# ========== 六、overview ==========
print('\n--- 六、overview ---')

out = run('register admin 123456\nlogin admin 123456\ncreate_pcb sn_a 2 1 3\ncreate_pcb sn_b 6 1 3\nalloc 64 2\noverview\nexit\n')
for s in ['Process Tree', 'Memory Map', '多级反馈队列', 'Stats', '就绪']:
    check_out(f'overview {s}', out, s)

# ========== 八、边界测试 ==========
print('\n--- 八、边界测试 ---')

check('8.1 非法参数', 'register admin 123456\nlogin admin 123456\ncreate_pcb\nkill_pcb 999\nalloc -1 1\nexit\n', '用法', '不存在', '必须在')
check('8.3 kill init', 'register admin 123456\nlogin admin 123456\nkill_pcb 1\nexit\n', '撤销失败')

# log_on / log_off
out = run('register admin 123456\nlogin admin 123456\nlog_on\ncreate_pcb lp 5 1 5\nlog_off\nexit\n')
check_out('log_on 前台', out, '[前台]')
check_out('log_on 后台', out, '[后台]')
check_out('log_off', out, '线程日志已关闭')

# 用户隔离
out = run('register aa 111\nregister bb 222\nlogin aa 111\ncreate_pcb aa_proc 1 1 10\nexit\n', keep_data=True)
out2 = run('login bb 222\nlist_pcb\nexit\n', keep_data=True)
check_out('用户隔离 bb看不到aa的进程', out2, neg=['aa_proc'])

# ========== 结果 ==========
print('\n' + '=' * 60)
print(f'通过 {passed} 项, 失败 {failed} 项')
print('=' * 60)
if errors:
    print('\n失败:')
    for e in errors:
        print(f'  {e}')

clean()
sys.exit(0 if failed == 0 else 1)

with open('测试流程.md', 'r', encoding='utf-8') as f:
    lines = f.readlines()

cmd_kw = ['register', 'login', 'logout', 'create_pcb', 'kill_pcb', 'block_pcb',
          'wakeup_pcb', 'show_pcb', 'list_pcb', 'ptree', 'suspend', 'resume',
          'renice', 'start_sched', 'stop_sched', 'restart_sched', 'step',
          'alloc', 'free_mem', 'show_mem', 'compact', 'mem_stat', 'set_alloc_algo',
          'pgfault', 'swap_out', 'save', 'load', 'overview', 'clear_save',
          '.\\', 'cd ', '# 窗口', '# 终端', 'kill_pcb 3', 'kill_pcb 4']

out = []
for i, l in enumerate(lines):
    s = l.rstrip('\n')
    if s == '```' and i + 1 < len(lines) and lines[i + 1].strip():
        nxt = lines[i + 1].strip()
        is_bash = any(nxt.startswith(k) for k in cmd_kw)
        out.append('```bash' if is_bash else '```text')
    else:
        out.append(s)

with open('测试流程.md', 'w', encoding='utf-8') as f:
    f.write('\n'.join(out))
print('done')

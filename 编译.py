#!/usr/bin/env python3
"""用MSVC编译OS模拟器"""
import os, subprocess, sys

# 强制Python使用UTF-8
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
sys.stderr.reconfigure(encoding='utf-8', errors='replace')

vc_dir = r'D:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231'
sdk_dir = r'C:\Program Files (x86)\Windows Kits\10'
sdk_ver = '10.0.26100.0'
base = os.path.dirname(os.path.abspath(__file__))
src_dir = os.path.join(base, '源码')
out_dir = os.path.join(base, '生成')
os.makedirs(out_dir, exist_ok=True)

# 收集cpp文件
cpp_files = []
for f in sorted(os.listdir(src_dir)):
    if f.endswith('.cpp'):
        cpp_files.append(os.path.join(src_dir, f))

print(f"Found {len(cpp_files)} source files")

# 设置环境
os.environ['INCLUDE'] = ';'.join([
    os.path.join(vc_dir, 'include'),
    os.path.join(sdk_dir, 'Include', sdk_ver, 'ucrt'),
    os.path.join(sdk_dir, 'Include', sdk_ver, 'um'),
    os.path.join(sdk_dir, 'Include', sdk_ver, 'shared'),
])
os.environ['LIB'] = ';'.join([
    os.path.join(vc_dir, 'lib', 'x64'),
    os.path.join(sdk_dir, 'Lib', sdk_ver, 'ucrt', 'x64'),
    os.path.join(sdk_dir, 'Lib', sdk_ver, 'um', 'x64'),
])
os.environ['PATH'] = os.path.join(vc_dir, 'bin', 'Hostx64', 'x64') + ';' + os.environ.get('PATH', '')

cl_exe = os.path.join(vc_dir, 'bin', 'Hostx64', 'x64', 'cl.exe')
out_exe = os.path.join(out_dir, '操作系统模拟器.exe')

obj_dir = os.path.join(out_dir, 'obj\\')
os.makedirs(obj_dir, exist_ok=True)

cmd = [cl_exe, '/nologo', '/EHsc', '/std:c++17', '/utf-8',
       f'/I{src_dir}', f'/Fe{out_exe}', f'/Fo{obj_dir}'] + cpp_files

print("Compiling...")
result = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace')
if result.stdout:
    print(result.stdout)
if result.stderr:
    print(result.stderr, file=sys.stderr)
print(f"Exit code: {result.returncode}")
sys.exit(result.returncode)

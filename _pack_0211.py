# 打包:ladybug-0.21.1-windows.zip + ladybug-0.21.1-ubuntu.zip
# 遵循 _pack_0202.py 约定:python zipfile;二进制 STORED、文本 DEFLATE;mtime 归一 1980-01-01。
import os, zipfile

ROOT = os.path.dirname(os.path.abspath(__file__))
RELEASES = os.path.join(ROOT, 'releases', '0.21.1')
os.makedirs(RELEASES, exist_ok=True)

EPOCH = (1980, 1, 1, 0, 0, 0)
BIN_EXT = {'.exe', '.lbug_extension', '.node', '.dll', '.so', '.a', '.lib', '.bin', '.wasm'}
# 排除:构建目录 / git 元数据 / oracle 虚拟环境 / 历史产物
SKIP_DIRS = {'build', 'build_v0211', 'build_v0211t', 'build_hyperalgo', '__pycache__', '.git',
             '.venv-oracle', 'releases', 'vendor'}
SKIP_PREFIXES = ('build', '_pack_', '_tmp')

def is_binary(p):
    return os.path.splitext(p)[1].lower() in BIN_EXT

def zip_path(zf, abspath, arcname):
    zi = zipfile.ZipInfo(arcname, EPOCH)
    zi.compress_type = zipfile.ZIP_STORED if is_binary(abspath) else zipfile.ZIP_DEFLATED
    zi.external_attr = 0o100644 << 16 | (0o100755 << 16 if os.access(abspath, os.X_OK) else 0)
    with open(abspath, 'rb') as f:
        zf.writestr(zi, f.read())

def walk(dist, prefix, zf, exclude_dirs=None):
    exclude_dirs = exclude_dirs or set()
    for dirpath, dirnames, filenames in os.walk(dist):
        dirnames[:] = [d for d in dirnames
                       if d not in SKIP_DIRS and d not in exclude_dirs
                       and not d.startswith(SKIP_PREFIXES)]
        for fn in filenames:
            if fn.startswith('build_wasm_') or fn.endswith('.log'):
                continue
            ap = os.path.join(dirpath, fn)
            rel = os.path.relpath(ap, dist)
            zip_path(zf, ap, prefix + rel.replace(os.sep, '/'))

# ---------- windows zip: dist-ladybug-0.21.1/(含 wasm-deploy) ----------
wzip = os.path.join(RELEASES, 'ladybug-0.21.1-windows.zip')
dist_dir = os.path.join(ROOT, 'dist-ladybug-0.21.1')
with zipfile.ZipFile(wzip, 'w', allowZip64=True) as zf:
    walk(dist_dir, 'dist-ladybug-0.21.1/', zf)
print('windows zip:', wzip, os.path.getsize(wzip))

# ---------- ubuntu zip: 整树快照(对齐 0.20.2 惯例:含 untracked/node_modules),
# 前缀 ladybug-0.21.1/, 仅排 build*/dist/benchmark/log
uzip = os.path.join(RELEASES, 'ladybug-0.21.1-ubuntu.zip')
with zipfile.ZipFile(uzip, 'w', allowZip64=True) as zf:
    walk(ROOT, 'ladybug-0.21.1/', zf,
         exclude_dirs={'dist-ladybug-0.21.1', 'benchmark'})
print('ubuntu zip:', uzip, os.path.getsize(uzip))
print('DONE')

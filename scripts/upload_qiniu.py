# -*- coding: utf-8 -*-
# flake8: noqa
#
# 打包产物上传七牛云。
#
# 用法:
#   python scripts/upload_qiniu.py [仓库根目录]
#   不带参数时默认取脚本所在目录上一级的 build/。
#   密钥从环境变量 QINIU_AK / QINIU_SK 读取（Windows: setx，Linux/macOS: shell rc），
#   未配置时仅做产物识别（干跑），不上传。
#
# 产物识别规则（与 scripts/package-* 产出一致，均在 build/ 下）:
#   Windows : seimi-render-<版本>-win-<arch>.zip
#   macOS   : seimi-render-<版本>-macos-<arch>.zip
#   Linux   : seimi-render-<版本>-linux-<arch>.tar.gz
# 命中多个时取 mtime 最新的一个。

import os
import platform
import sys
import time
from pathlib import Path

from qiniu import Auth, put_file_v2, etag
import qiniu.config  # noqa: F401

# ==== 七牛配置（密钥走环境变量 QINIU_AK / QINIU_SK）====
access_key = os.environ.get('QINIU_AK', '')
secret_key = os.environ.get('QINIU_SK', '')
bucket_name = 'seimicrawler'
key_prefix = 'seimirender/'  # 上传 key 前缀，文件名与打包产物名保持一致

TOKEN_TTL = 3600  # 上传 Token 有效期（秒）

ARTIFACT_PATTERNS = {
    'Windows': 'seimi-render-*-win-*.zip',
    'Darwin': 'seimi-render-*-macos-*.zip',
    'Linux': 'seimi-render-*-linux-*.tar.gz',
}

PACKAGE_HINT = {
    'Windows': r'scripts\package-windows.bat',
    'Darwin': 'bash scripts/package.sh Release',
    'Linux': 'bash scripts/package-linux.sh',
}


def find_artifact(build_dir, system):
    pattern = ARTIFACT_PATTERNS.get(system)
    if not pattern:
        raise SystemExit('[ERROR] 不支持的平台: %s' % system)
    candidates = sorted(build_dir.glob(pattern),
                        key=lambda p: p.stat().st_mtime, reverse=True)
    if not candidates:
        raise SystemExit('[ERROR] %s 下未找到 %s\n       请先打包: %s'
                         % (build_dir, pattern, PACKAGE_HINT[system]))
    if len(candidates) > 1:
        print('[WARN] 命中 %d 个产物，取最新的，其余忽略:' % len(candidates))
        for p in candidates:
            mtime = time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(p.stat().st_mtime))
            print('    %s  %s' % (mtime, p.name))
    return candidates[0]


def make_progress_handler():
    # SDK 回调签名: progress_handler(uploaded_size, total_size)，单位字节
    last_step = [-1]

    def on_progress(uploaded, total, *args):
        try:
            uploaded, total = float(uploaded), float(total)
        except (TypeError, ValueError):
            return
        if total <= 0:
            return
        pct = min(100, int(uploaded * 100 / total))
        step = pct // 10
        if step != last_step[0]:
            last_step[0] = step
            print('  upload %d%%' % pct)

    return on_progress


def main():
    repo_root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 \
        else Path(__file__).resolve().parent.parent  # scripts/ 的上一级即仓库根
    build_dir = repo_root / 'build'
    system = platform.system()
    print('platform : %s' % system)
    print('build    : %s' % build_dir)

    localfile = find_artifact(build_dir, system)
    size_mb = localfile.stat().st_size / 1024 / 1024
    print('artifact : %s (%.1f MB)' % (localfile.name, size_mb))

    key = key_prefix + localfile.name
    print('target   : %s/%s （同名覆盖）' % (bucket_name, key))

    if access_key in ('', 'Access_Key') or secret_key in ('', 'Secret_Key'):
        print('[SKIP] 未配置 Access Key / Secret Key，仅识别产物，不上传。')
        return

    q = Auth(access_key, secret_key)
    # scope 用 bucket:key，同 key 再传即覆盖
    token = q.upload_token(bucket_name, key, TOKEN_TTL)

    ret, info = put_file_v2(token, key, str(localfile), version='v2',
                            progress_handler=make_progress_handler())
    print(info)
    if ret is None:
        raise SystemExit('[ERROR] 上传失败: %s' % info)
    assert ret['key'] == key, 'key 不一致: %s != %s' % (ret['key'], key)
    assert ret['hash'] == etag(str(localfile)), 'hash 校验不一致'
    print('[OK] 已上传: %s/%s' % (bucket_name, key))


if __name__ == '__main__':
    main()

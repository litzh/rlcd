#!/usr/bin/env sh
# 安装 / 更新 dsh-rlcd 插件到 $DSH_HOME/plugins/dsh-rlcd/。
#
#   ./install.sh
#
# 只复制文件，不会改写已有的 $DSH_HOME/cordis.patch.yml；
# 若 home patch 里还没有 dsh-rlcd，会打印需要添加的内容。
set -eu

home="${DSH_HOME:-$HOME/.dsh}"
target="$home/plugins/dsh-rlcd"
here="$(cd "$(dirname "$0")" && pwd)"

mkdir -p "$target"
cp "$here/index.ts" "$here/smoke-test.ts" "$here/README.md" "$target/"

echo "已安装到 $target"

patch="$home/cordis.patch.yml"
if [ -f "$patch" ] && grep -q 'dsh-rlcd' "$patch"; then
  echo "已注册：$patch"
  echo "改完源码后重启 dsh web 生效（patch 是 config-only HMR，不会重载插件源码）。"
  exit 0
fi

echo
echo "还需要在 $patch 里注册本插件："
echo
echo "- insert:"
echo "    - id: dsh-rlcd"
echo "      name: '$target/index.ts'"

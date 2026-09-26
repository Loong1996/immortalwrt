#!/bin/sh
# Run U-Boot's net/tcp.c on the host against a simulated browser.
#
#   run.sh <upstream tree> <patched tree>
#
# Both are unpacked u-boot-$(PKG_VERSION) trees; the second has had src/ and
# patches/ applied.  Only net/tcp.c and include/net/tcp.h are used from them.
#
# Checks:
#   1. download (dl): the patched stack delivers every byte and closes cleanly
#      across sizes, data loss and ACK loss
#   2. upload + small answer (ul), the shape of every flashing page: both
#      stacks get every byte in, answer, "act" exactly once and close
#   3. answers of 9 and 200 bytes ({"ok":1}, errors, /wr) come out identical
#      on both stacks, timing included -- a change there changes flashing
#   4. the model still shows the bug 204 fixes (upstream < 100 KiB/s with a
#      40 ms delayed ACK) and the patched stack is at least 10x faster
#   5. download into a small, slowly read window, with and without a window
#      scale offered in the SYN, with loss and reordering: the patched stack
#      never sends past the window the peer advertised (the SYN-ACK offers
#      no scale, so none applies)
#
# The model has no flash reads and no NIC; speeds here are not real speeds.
set -eu

ORIG=$1
NEW=$2
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

build() {	# build <old|new> <tree>
	# Just the two files: the tree's include/ would shadow the host's
	# stdio.h and string.h.
	mkdir -p "$OUT/$1/net" "$OUT/$1/include/net"
	cp "$2/net/tcp.c" "$OUT/$1/net/"
	cp "$2/include/net/tcp.h" "$OUT/$1/include/net/"
	for s in dl ul; do
		gcc -O2 -w -I"$HERE/stub" -I"$OUT/$1/include" -o "$OUT/$s-$1" \
			"$HERE/$s.c" "$OUT/$1/net/tcp.c"
	done
}
build old "$ORIG"
build new "$NEW"

fail=0
err() {
	echo "::error::$*"
	fail=1
}

# 1. download, patched stack
n=0
for size in 1 1448 1449 65536 1048576 8388608; do
	for loss in 0 0.01 0.05; do
		for ackloss in 0 0.1; do
			for seed in 1 2; do
				n=$((n + 1))
				r=$("$OUT/dl-new" $size $loss $ackloss $seed 40) ||
					err "下载 size=$size loss=$loss ackloss=$ackloss seed=$seed: $r"
			done
		done
	done
done
echo "1. 下载：$n 组"

# 2 + 3. upload + answer, both stacks
n=0
same=0
for up in 100 65536 1048576; do
	for resp in 9 200 1449 5000; do
		for loss in 0 0.01 0.05; do
			for ackloss in 0 0.05; do
				for early in 0 1; do
					for seed in 1 2; do
						args="$up $resp $loss $ackloss $seed $early"
						n=$((n + 1))
						o=$("$OUT/ul-old" $args) ||
							err "上传（上游）$args: $o"
						r=$("$OUT/ul-new" $args) ||
							err "上传（打过补丁）$args: $r"
						[ $resp -gt 200 ] && continue
						if [ "$o" = "$r" ]; then
							same=$((same + 1))
						else
							err "小响应与上游不一致 $args"
							echo "  上游：$o"
							echo "  补丁：$r"
						fi
					done
				done
			done
		done
	done
done
echo "2. 上传加响应：$n 组，两个栈都跑"
echo "3. 9 B / 200 B 响应与上游逐组相同：$same 组"

# 4. the bug is still in the model, and it is fixed
rate() {
	sed -n 's/.* = \([0-9.]*\) KiB\/s.*/\1/p'
}
o=$("$OUT/dl-old" 1048576 0 0 1 40 | rate) || true
r=$("$OUT/dl-new" 1048576 0 0 1 40 | rate) || true
echo "4. 1 MiB、40 ms 延迟确认：上游 $o KiB/s，打过补丁 $r KiB/s"
# 没取到速率（程序崩了、输出格式变了）时下面两条比较会把空串当 0 放过去
[ -n "$o" ] && [ -n "$r" ] ||
	err "第 4 项没取到速率（上游「$o」，打过补丁「$r」）：dl 没跑完或输出格式变了"
awk -v o="$o" 'BEGIN { exit !(o < 100) }' ||
	err "上游在模型里不慢了（$o KiB/s）：模型变了，这组检查已经证明不了什么"
awk -v o="$o" -v r="$r" 'BEGIN { exit !(r >= 10 * o) }' ||
	err "打过补丁的栈不比上游快 10 倍（$r 对 $o KiB/s）：204 丢了或者坏了"

# 5. small, slowly read windows
n=0
for read in 0 5000 1000; do
	for ws in -1 7; do
		for loss in 0 0.01; do
			for reorder in 0 0.05; do
				args="1048576 $loss 0 1 40 8192 $ws $reorder $read"
				n=$((n + 1))
				r=$("$OUT/dl-new" $args) ||
					err "小窗口下载 $args: $r"
				case "$r" in
				*"overwin 0") ;;
				*) err "发出了对端窗口以外的段 $args: $r" ;;
				esac
			done
		done
	done
done
echo "5. 小窗口、慢读、窗口缩放、乱序：$n 组"

[ $fail = 0 ] && echo "全部通过"
exit $fail

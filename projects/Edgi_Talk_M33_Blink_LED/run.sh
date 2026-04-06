#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
OPENOCD_ROOT="$(cd "$SDK_ROOT/../tools/openocd" && pwd)"
OPENOCD="$OPENOCD_ROOT/bin/openocd"

: "${RTT_EXEC_PATH:=/home/huan/toolchain/arm-eabi-toolchain/bin}"
export RTT_EXEC_PATH

BOOT_CONFIG="$SCRIPT_DIR/config/boot_with_extended_boot_scons.json"
FLASH_HEX="$SCRIPT_DIR/build/rtthread.hex"
JOBS=$(nproc 2>/dev/null || echo 4)

do_build() {
    echo "==> [1/2] scons 编译 ..."
    cd "$SCRIPT_DIR"
    scons -j"$JOBS"

    echo "==> [2/2] 固件后处理 (hex-relocate + merge S-side) ..."
    edgeprotecttools run-config --input "$BOOT_CONFIG"

    echo "==> 编译完成: $FLASH_HEX"
}

do_flash() {
    if [ ! -f "$FLASH_HEX" ]; then
        echo "错误: 未找到 $FLASH_HEX，请先执行 $0 build"
        exit 1
    fi

    echo "==> 使用 Infineon OpenOCD 烧录: $FLASH_HEX"
    "$OPENOCD" \
        -s "$OPENOCD_ROOT/scripts" \
        -f interface/kitprog3.cfg \
        -f target/infineon/pse84xgxs2.cfg \
        -c "transport select swd" \
        -c "cat1d.cm33 configure -rtos auto -rtos-wipe-on-reset-halt 1" \
        -c "gdb_breakpoint_override hard" \
        -c "init; reset init; adapter speed 12000" \
        -c "flash write_image erase $FLASH_HEX" \
        -c "reset run" \
        -c "shutdown"
    echo "==> 烧录完成"
}

do_clean() {
    echo "==> 清理编译产物 ..."
    cd "$SCRIPT_DIR"
    scons -c
    rm -rf build
    echo "==> 清理完成"
}

case "${1:-}" in
    build)
        do_build
        ;;
    flash)
        do_flash
        ;;
    clean)
        do_clean
        ;;
    all)
        do_build
        do_flash
        ;;
    *)
        echo "用法: $0 {build|flash|clean|all}"
        echo ""
        echo "  build  - 编译 + 固件后处理"
        echo "  flash  - 烧录到开发板 (KitProg3/DAP)"
        echo "  clean  - 清理编译产物"
        echo "  all    - 编译 + 烧录"
        exit 1
        ;;
esac

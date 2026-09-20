#!/bin/bash
set -e
set -o pipefail

# Configuration
DIR=$(readlink -f .)
MAIN=$(readlink -f ${DIR}/..)
KERNEL_DEFCONFIG=raccoon_defconfig
CLANG_DIR="$MAIN/toolchains/clang"
KERNEL_DIR=$(pwd)
OUT_DIR="$KERNEL_DIR/out"
ZIMAGE_DIR="$OUT_DIR/arch/arm64/boot"
DTB_DTBO_DIR="$ZIMAGE_DIR/dts/vendor/qcom"
BUILD_START=$(date +"%s")

# Logging configuration
TIME=$(date "+%Y%m%d-%H%M%S")
LOG_DIR="$KERNEL_DIR/build_logs"
LOG_FILE="$LOG_DIR/build-$TIME.log"
LOGS_TO_KEEP=10   # set to 0 to keep everything

# Telegram configuration
TOKEN="${TELEGRAM_BOT_TOKEN}"
CHATID="${TELEGRAM_CHAT_ID}"
BOT_MSG_URL="https://api.telegram.org/bot$TOKEN/sendMessage"
BOT_BUILD_URL="https://api.telegram.org/bot$TOKEN/sendDocument"

mkdir -p "$LOG_DIR"

# Mirror everything (stdout+stderr) to the log file as well as the terminal
exec > >(tee -a "$LOG_FILE") 2>&1

echo "=========================================="
echo "Build log: $LOG_FILE"
echo "Started:   $(date)"
echo "=========================================="

# Send a text message to Telegram
tg_post_msg() {
    if [ -z "$TOKEN" ] || [ -z "$CHATID" ]; then
        return 0
    fi
    curl -s -H "Content-Type: application/x-www-form-urlencoded" -X POST "$BOT_MSG_URL" \
        -d chat_id="$CHATID" \
        -d "disable_web_page_preview=true" \
        -d "parse_mode=html" \
        -d text="$1" >/dev/null
}

# Send a file (e.g. the zip or the log) to Telegram
tg_post_build() {
    if [ -z "$TOKEN" ] || [ -z "$CHATID" ]; then
        return 0
    fi
    local MD5CHECK
    MD5CHECK=$(md5sum "$1" | cut -d' ' -f1)
    curl --progress-bar -F document=@"$1" "$BOT_BUILD_URL" \
        -F chat_id="$CHATID" \
        -F "disable_web_page_preview=true" \
        -F "parse_mode=Markdown" \
        -F caption="$2 | *MD5 Checksum : *\`$MD5CHECK\`" >/dev/null
}

tg_post_msg "<b>Kernel build started</b>%0A<b>Date:</b> <code>$(date)</code>"

sudo apt install libelf-dev -y

# Function to check for existing Clang
check_clang() {
    if [ -d "$CLANG_DIR" ] && [ -f "$CLANG_DIR/bin/clang" ]; then
        export PATH="$CLANG_DIR/bin:$PATH"
        export KBUILD_COMPILER_STRING="$($CLANG_DIR/bin/clang --version | head -n 1 | perl -pe 's/\(http.*?\)//gs' | sed -e 's/  */ /g' -e 's/[[:space:]]*$//')"
        echo "Found existing Clang: $KBUILD_COMPILER_STRING"
        return 0
    fi
    return 1
}

# Install Clang if needed
if ! check_clang; then
    echo "No valid Clang found. Installing..."
    echo "1. Zyc Clang 16.0.6"
    echo "2. Prelude Clang 16.0.2"
    read -p "Choose [1-2]: " clang_choice

    case "$clang_choice" in
        1)
            CLANG_URL="https://github.com/ZyCromerZ/Clang/releases/download/16.0.6-20250721-release/Clang-16.0.6-20250721.tar.gz"
            ARCHIVE_NAME="clang.tar.gz"
            mkdir -p "$CLANG_DIR"
            wget -P "$MAIN" "$CLANG_URL" -O "$MAIN/$ARCHIVE_NAME" || exit 1
            tar -xf "$MAIN/$ARCHIVE_NAME" -C "$CLANG_DIR" || exit 1
            rm -f "$MAIN/$ARCHIVE_NAME"
            ;;
        2)
            git clone --depth=1 https://gitlab.com/jjpprrrr/prelude-clang.git "$CLANG_DIR" || exit 1
            ;;
        *)
            echo "Invalid choice. Exiting..."
            exit 1
            ;;
    esac

    if ! check_clang; then
        echo "Clang installation failed. Exiting..."
        exit 1
    fi
fi

# Set up toolchain
export ARCH=arm64
export SUBARCH=arm64
export CROSS_COMPILE=aarch64-linux-gnu-

# Trap failures so we know where to look for the log, and where the build stopped
on_error() {
    local exit_code=$?
    echo -e "\n=========================================="
    echo "Build FAILED (exit code $exit_code)"
    echo "See full log: $LOG_FILE"
    echo "=========================================="
    tg_post_build "$LOG_FILE" "Build failed (exit code $exit_code)"
    exit "$exit_code"
}
trap on_error ERR

# Build kernel
echo -e "\n--- Cleaning source tree ---"
make ARCH=arm64 CC=clang LLVM=1 LLVM_IAS=1 mrproper
rm -rf "$OUT_DIR"

echo -e "\n--- Generating defconfig ---"
make O="$OUT_DIR" CC=clang LLVM=1 LLVM_IAS=1 KCFLAGS="-w" $KERNEL_DEFCONFIG

echo -e "\n--- Compiling kernel ---"
make -kj"$(nproc --all)" O="$OUT_DIR" CC=clang LLVM=1 LLVM_IAS=1 KCFLAGS="-w"

# Build succeeded past this point, so remove the trap
trap - ERR

# Clean up old kernel zip files
echo "Cleaning up old kernel zip files..."
find "$KERNEL_DIR" -maxdepth 1 -type f -name "Raccoon-66-*.zip" -exec rm -v {} \;

# Create temporary anykernel directory
TEMP_ANY_KERNEL_DIR="$KERNEL_DIR/anykernel_temp"
rm -rf "$TEMP_ANY_KERNEL_DIR"

# Clone entire anykernel directory
echo "Cloning anykernel directory..."
if [ -d "$KERNEL_DIR/anykernel" ]; then
    cp -r "$KERNEL_DIR/anykernel" "$TEMP_ANY_KERNEL_DIR"
else
    echo "Error: anykernel directory not found!"
    exit 1
fi

# Copy kernel image
if [ -f "$ZIMAGE_DIR/Image.gz-dtb" ]; then
    cp -v "$ZIMAGE_DIR/Image.gz-dtb" "$TEMP_ANY_KERNEL_DIR/"
elif [ -f "$ZIMAGE_DIR/Image.gz" ]; then
    cp -v "$ZIMAGE_DIR/Image.gz" "$TEMP_ANY_KERNEL_DIR/"
elif [ -f "$ZIMAGE_DIR/Image" ]; then
    cp -v "$ZIMAGE_DIR/Image" "$TEMP_ANY_KERNEL_DIR/"
fi

# Create zip file in kernel root directory
echo "Creating zip package..."
ZIP_NAME="Raccoon-66-$TIME.zip"
cd "$TEMP_ANY_KERNEL_DIR"
zip -r9 "$KERNEL_DIR/$ZIP_NAME" ./*
cd ..

# Clean up temporary directory
rm -rf "$TEMP_ANY_KERNEL_DIR"

# Trim old logs, keeping only the most recent LOGS_TO_KEEP
if [ "$LOGS_TO_KEEP" -gt 0 ]; then
    ls -1t "$LOG_DIR"/build-*.log 2>/dev/null | tail -n +"$((LOGS_TO_KEEP + 1))" | xargs -r rm -v
fi

BUILD_END=$(date +"%s")
DIFF=$((BUILD_END - BUILD_START))
echo -e "\n=========================================="
echo "Build completed in $((DIFF / 60))m $((DIFF % 60))s"
echo "Final zip: $KERNEL_DIR/$ZIP_NAME"
echo "Zip size: $(du -h "$KERNEL_DIR/$ZIP_NAME" | cut -f1)"
echo "Build log: $LOG_FILE"
echo "=========================================="

tg_post_build "$KERNEL_DIR/$ZIP_NAME" "Build took $((DIFF / 60))m $((DIFF % 60))s"
tg_post_build "$LOG_FILE" "Compile log"
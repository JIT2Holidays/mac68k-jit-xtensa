#!/usr/bin/env bash
# Source this to get a working ESP-IDF v6.0.1 environment.
#
# The stock export.sh derives the Python venv path from the live `python3`
# version, which no longer matches the installed venv; the bundled
# activate script `exit 1`s when sourced from inside another script
# (its is-sourced check inspects $0). So we set the environment directly
# from the known install layout — globs absorb minor version bumps.

_ESP="$HOME/.espressif"

export IDF_PATH="$_ESP/v6.0.1/esp-idf"
export IDF_TOOLS_PATH="$_ESP/tools"
export ESP_IDF_VERSION="6.0.1"

# Python virtual environment (version-pinned path used by IDF v6).
for v in "$_ESP"/tools/python/v6.0.1/venv "$_ESP"/python_env/*; do
    [ -x "$v/bin/python" ] && export IDF_PYTHON_ENV_PATH="$v" && break
done

for d in "$IDF_PYTHON_ENV_PATH"/bin \
         "$_ESP"/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin \
         "$_ESP"/tools/xtensa-esp-elf-gdb/*/xtensa-esp-elf-gdb/bin \
         "$_ESP"/tools/cmake/*/CMake.app/Contents/bin \
         "$_ESP"/tools/ninja/* \
         "$_ESP"/tools/qemu-xtensa/*/qemu/bin; do
    [ -d "$d" ] && PATH="$d:$PATH"
done
export PATH

for r in "$_ESP"/tools/esp-rom-elfs/*; do
    [ -d "$r" ] && export ESP_ROM_ELF_DIR="$r" && break
done

# idf.py runs through the venv python.
idf.py() {
    "$IDF_PYTHON_ENV_PATH/bin/python" "$IDF_PATH/tools/idf.py" "$@"
}
export -f idf.py 2>/dev/null || true

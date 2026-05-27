#!/bin/sh
# Sweep SE/30 ROM with various SCC byte streams (Macsbug-like commands)
# to identify which advance boot meaningfully. The SE/30 ROM has a
# command parser in its post-PMMU phase (see M7.6as) — each command
# yields a different boot state.
#
# Usage: ./scripts/se30-cmd-sweep.sh [cycles=100000000]
# Prints one line per stream tested with the final PC, D7, fault_addr.

set -e
cd "$(dirname "$0")/.."

CYC=${1:-500000000}
echo "Sweeping SE/30 commands at $CYC cycles each."
echo "Format: <stream>  PC  D7  fault_addr  instrs"
echo "---------------------------------------------"

# Each entry: <hex stream>  <description>
# Letters in ASCII: G=47 S=53 R=52 T=54 B=42 D=44 M=4D Q=51
#                   P=50 L=4C E=45 I=49 V=56 W=57 X=58
sweep() {
    local stream=$1
    local desc=$2
    local result=$(SE30_PATCH_32AC=1 \
                   SE30_INJECT_BYTE=0x2A \
                   SE30_INJECT_AT=10000000 \
                   SE30_INJECT_STREAM=$stream \
                   ./build/se30_trace $CYC 2>&1 \
                   | grep "halt" | head -1)
    pc=$(echo "$result" | grep -oE 'pc=0x[0-9A-F]+' | head -1)
    d7=$(echo "$result" | grep -oE 'd7=0x[0-9A-F]+' | head -1)
    fa=$(echo "$result" | grep -oE 'last_fault_addr=0x[0-9A-F]+' | head -1)
    ins=$(echo "$result" | grep -oE 'instrs=[0-9]+' | head -1)
    printf '%-20s %-30s %s %s %s %s\n' "$stream" "$desc" "$pc" "$d7" "$fa" "$ins"
}

# baseline — no stream
sweep "" "(no stream)"

# Single-character commands
sweep "2A472A"      "*G*  Macsbug Go"
sweep "2A532A"      "*S*  Stop"
sweep "2A522A"      "*R*  Restart"
sweep "2A542A"      "*T*  Trace"
sweep "2A422A"      "*B*  Breakpoint"
sweep "2A4D2A"      "*M*  Memory"
sweep "2A442A"      "*D*  Display"
sweep "2A4C2A"      "*L*  List"
sweep "2A4E2A"      "*N*  Next"
sweep "2A572A"      "*W*  Watch"

# Two-letter commands
sweep "2A47532A"    "*GS* Go + Stop"
sweep "2A444D2A"    "*DM* Display Memory"
sweep "2A45532A"    "*ES* Exit Shell"
sweep "2A52532A"    "*RS* Restart System"
sweep "2A52422A"    "*RB* Reboot"
sweep "2A50522A"    "*PR* Print"
sweep "2A53492A"    "*SI* Step Into"
sweep "2A534F2A"    "*SO* Step Over"
sweep "2A57422A"    "*WB* Write Byte"

# Three-letter
sweep "2A4252422A"  "*BRB* boot"
sweep "2A47472A2A"  "*GG**"

# CR-terminated common
sweep "2A4720300D"  "*G 0\\r — Go to addr 0"
sweep "2A4720340D"  "*G 4\\r — Go to addr 4"

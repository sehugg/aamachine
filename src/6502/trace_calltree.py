#!/usr/bin/env python3
"""Build a call tree from an izconsole "-trace cpu" log, symbolized with
a2.labels (or any similarly-formatted labels file).

Typical use — pipe a live trace straight in and watch it build:

    ~/bin/izconsole -trace cpu -speed full disk.po \\
        | python3 src/6502/trace_calltree.py --labels src/6502/a2.labels

...or point it at a saved trace file:

    python3 src/6502/trace_calltree.py my_trace.log

Press Ctrl-C at any point (or hit --max-instructions / --max-seconds /
--stop-pc) and it dumps the call tree built so far.

If izconsole is piped through another process (like this one) its stdout
may become fully buffered instead of line-buffered, which delays "real
time" feedback. If updates seem to stall, try `stdbuf -oL izconsole ...`
or run izconsole against a saved log file instead.

6502 stacks can do things a naive JSR/RTS pairing can't represent: code
that pushes a fake return address and RTSes into it, code that discards a
return address with PLA/PLA, code that resets SP directly (TXS) to unwind
several frames at once, IRQ/BRK/RTI, and outright recursion. Instead of
matching JSR against RTS one at a time, this script tracks the actual
stack pointer and pops a call-tree frame whenever SP rises past the point
it was at when that frame was entered — whether that happened via a
matching RTS, a multi-level unwind, or something stranger. Non-matching
returns are counted as anomalies per node rather than crashing the parser.
"""

import argparse
import collections
import os
import re
import sys
import time

LINE_RE = re.compile(
    r'^0x(?P<addr>[0-9a-fA-F]+)\s+(?P<instr>.*?)\s*:\s*'
    r'A:\s*0x[0-9a-fA-F]+\([^)]*\),\s*'
    r'X:\s*0x[0-9a-fA-F]+,\s*'
    r'Y:\s*0x[0-9a-fA-F]+,\s*'
    r'SP:\s*0x(?P<sp>[0-9a-fA-F]+),\s*'
    r'PC:\s*0x(?P<pc>[0-9a-fA-F]+),\s*'
    r'P:\s*0x[0-9a-fA-F]+,\s*'
    r'\(NV-BDIZC\):\s*[01]{8},\s*'
    r'R:\s*0x(?P<ea>[0-9a-fA-F]+),\s*'
    r'\[(?P<bytes>[0-9a-fA-F]*)\]\s*$'
)

# The trace's "R:" field is the CPU's resolved effective address for the
# current instruction -- already indirection-resolved (e.g. "STA ($f8,X)"
# shows the real target, not the zero-page pointer address). But it is only
# meaningful for addressing modes that actually reference memory: for
# implied, accumulator and immediate opcodes it is simply not updated (it
# carries whatever the last real memory reference left there), and for
# branches/JMP/JSR it holds the *branch/jump target* -- i.e. an instruction
# fetch, not a data access. So memory tracking below is opcode-classified
# (read/write/read-modify-write) and explicitly excludes control-flow
# mnemonics, rather than trusting "R:" wherever it appears.
CONTROL_MNEMONICS = (
    {'JMP', 'JSR', 'BRK', 'RTI', 'RTS', 'BRA'} |
    {'BCC', 'BCS', 'BEQ', 'BMI', 'BNE', 'BPL', 'BVC', 'BVS'} |
    {f'BBR{i}' for i in range(8)} | {f'BBS{i}' for i in range(8)}
)
MEM_READ = {'LDA', 'LDX', 'LDY', 'CMP', 'CPX', 'CPY', 'AND', 'ORA', 'EOR', 'ADC', 'SBC', 'BIT'}
MEM_WRITE = {'STA', 'STX', 'STY', 'STZ'}
MEM_RMW = {'INC', 'DEC', 'ASL', 'LSR', 'ROL', 'ROR', 'TRB', 'TSB'}


def classify_mem_access(mnem, operand):
    """Returns 'r', 'w', 'rw' or None (no data-memory access) for an
    instruction, given its mnemonic and the raw operand text (e.g. '$c011',
    '($f8,X)', '#$00', 'A', or '' for implied)."""
    if mnem in CONTROL_MNEMONICS:
        return None
    if not operand or operand == 'A' or operand.startswith('#'):
        return None
    if mnem in MEM_READ:
        return 'r'
    if mnem in MEM_WRITE:
        return 'w'
    if mnem in MEM_RMW:
        return 'rw'
    return None


def merge_ranges(counter, gap=0):
    """Coalesces a {addr: count} mapping into sorted (lo, hi, total_count)
    ranges, merging addresses separated by at most `gap` untouched bytes."""
    ranges = []
    lo = hi = None
    total = 0
    for a in sorted(counter):
        if lo is None:
            lo = hi = a
            total = counter[a]
        elif a - hi - 1 <= gap:
            hi = a
            total += counter[a]
        else:
            ranges.append((lo, hi, total))
            lo = hi = a
            total = counter[a]
    if lo is not None:
        ranges.append((lo, hi, total))
    return ranges



def load_labels(path):
    """Returns (addr2name, name2addr) from a `NAME, 0xADDR, scope, ...` labels
    file: addr2name maps an address to its best (preferring global-scope)
    symbol name; name2addr maps every name seen to its address, for resolving
    a symbol typed on the command line (e.g. --stop-pc)."""
    addr2names = collections.defaultdict(list)
    name2addr = {}
    if not path or not os.path.exists(path):
        return {}, {}
    with open(path) as f:
        for line in f:
            parts = [p.strip() for p in line.strip().split(',')]
            if len(parts) < 2 or not parts[0]:
                continue
            name = parts[0]
            try:
                addr = int(parts[1], 0)
            except ValueError:
                continue
            scope = 0
            if len(parts) >= 3:
                try:
                    scope = int(parts[2])
                except ValueError:
                    pass
            addr2names[addr].append((scope, name))
            name2addr.setdefault(name, addr)
    best = {}
    for addr, names in addr2names.items():
        names.sort(key=lambda t: (0 if t[0] == 0 else 1, t[1]))
        best[addr] = names[0][1]
    return best, name2addr


class Node:
    __slots__ = ('addr', 'call_site', 'kind', 'children', 'count',
                 'self_instr', 'anomalies', 'first_line')

    def __init__(self, addr, call_site, kind):
        self.addr = addr
        self.call_site = call_site
        self.kind = kind          # 'root' | 'jsr' | 'brk'
        self.children = {}        # call_site addr -> Node
        self.count = 0
        self.self_instr = 0
        self.anomalies = 0
        self.first_line = None

    def total_instr(self):
        return self.self_instr + sum(c.total_instr() for c in self.children.values())

    def total_calls(self):
        return self.count + sum(c.total_calls() for c in self.children.values())


class StackEntry:
    __slots__ = ('node', 'push_sp', 'pop_delta', 'expected_return', 'active_roots')

    def __init__(self, node, push_sp, pop_delta, expected_return, active_roots=frozenset()):
        self.node = node
        self.push_sp = push_sp
        self.pop_delta = pop_delta          # bytes popped by a matching return
        self.expected_return = expected_return
        self.active_roots = active_roots    # --track-mem addrs whose subtree we're in


class CallTree:
    def __init__(self, addr2name, track_targets=()):
        self.addr2name = addr2name
        self.root = Node(None, None, 'root')
        self.track_order = list(track_targets)      # --track-mem addrs, in given order
        self.tracked_addrs = set(self.track_order)
        self.mem_stats = collections.defaultdict(
            lambda: {'reads': collections.Counter(), 'writes': collections.Counter()})
        self.stack = [StackEntry(self.root, float('inf'), 0, None, frozenset())]
        self.instr_count = 0
        self.line_count = 0
        self.unparsed = 0
        self.anomaly_total = 0
        self.anomaly_log = collections.deque(maxlen=500)
        self.last_raw_sp = None   # last raw (0-255) SP seen, for unwrapping
        self.virtual_sp = None    # monotonically-unwrapped SP (see _unwrap_sp)
        self.start_time = time.monotonic()

    def _unwrap_sp(self, raw_sp):
        """The real 6502 stack pointer is one byte and wraps (0x00 <-> 0xff);
        page-1-only code (disk bit-banging loops in particular) runs with SP
        pinned near a wrap boundary. Comparing raw SP values across a wrap
        breaks frame matching, so track an unwrapped 'virtual SP' instead:
        each step, assume the smaller of the two possible deltas (direct or
        wrapped) is what really happened. This is unambiguous for ordinary
        push/pop deltas (at most 3, from BRK/RTI); a single instruction that
        legitimately moves SP by more than half a page (e.g. a big TXS-based
        stack reset) is rare enough to accept as a known limitation."""
        if self.last_raw_sp is None:
            self.last_raw_sp = raw_sp
            self.virtual_sp = raw_sp
            return self.virtual_sp
        delta = raw_sp - self.last_raw_sp
        if delta > 128:
            delta -= 256
        elif delta < -128:
            delta += 256
        self.virtual_sp += delta
        self.last_raw_sp = raw_sp
        return self.virtual_sp

    def symbol(self, addr):
        if addr is None:
            return '<root>'
        name = self.addr2name.get(addr)
        return f'{name} (${addr:04x})' if name else f'${addr:04x}'

    def feed_line(self, raw, lineno, live=False):
        m = LINE_RE.match(raw)
        if not m:
            self.unparsed += 1
            return
        self.line_count += 1
        self.instr_count += 1

        addr = int(m.group('addr'), 16)
        raw_sp = int(m.group('sp'), 16)
        sp = self._unwrap_sp(raw_sp)
        pc_after = int(m.group('pc'), 16)
        instr = m.group('instr').strip()
        mnem = instr.split()[0] if instr else ''
        operand = instr[len(mnem):].strip()
        nbytes = len(m.group('bytes')) // 2

        top = self.stack[-1]
        top.node.self_instr += 1

        if top.active_roots:
            kind = classify_mem_access(mnem, operand)
            if kind:
                ea = int(m.group('ea'), 16)
                for root in top.active_roots:
                    stats = self.mem_stats[root]
                    if kind in ('r', 'rw'):
                        stats['reads'][ea] += 1
                    if kind in ('w', 'rw'):
                        stats['writes'][ea] += 1

        if mnem == 'JSR':
            try:
                target = int(operand.lstrip('$'), 16)
            except ValueError:
                target = None
            child = top.node.children.get(addr)
            if child is None:
                child = Node(target, addr, 'jsr')
                child.first_line = lineno
                top.node.children[addr] = child
            child.count += 1
            active_roots = top.active_roots
            if target is not None and target in self.tracked_addrs:
                active_roots = active_roots | {target}
            entry = StackEntry(child, sp, 2, addr + max(nbytes, 3), active_roots)
            self.stack.append(entry)
            if live:
                sys.stderr.write('  ' * (len(self.stack) - 1) +
                                  f'-> {self.symbol(target)} from ${addr:04x}  [line {lineno}]\n')
        elif mnem == 'BRK':
            child = top.node.children.get(addr)
            if child is None:
                child = Node(None, addr, 'brk')
                child.first_line = lineno
                top.node.children[addr] = child
            child.count += 1
            self.stack.append(StackEntry(child, sp, 3, None, top.active_roots))
            if live:
                sys.stderr.write('  ' * (len(self.stack) - 1) +
                                  f'-> BRK at ${addr:04x}  [line {lineno}]\n')

        while len(self.stack) > 1 and self.stack[-1].push_sp < sp:
            entry = self.stack.pop()
            normal = (mnem in ('RTS', 'RTI')
                      and sp == entry.push_sp + entry.pop_delta
                      and (entry.expected_return is None or pc_after == entry.expected_return))
            if not normal:
                entry.node.anomalies += 1
                self.anomaly_total += 1
                reason = 'non-standard return' if mnem in ('RTS', 'RTI') else 'frame abandoned (no RTS)'
                self.anomaly_log.append((lineno, addr, mnem, reason))
            if live:
                tag = '' if normal else '  [ANOMALY]'
                sys.stderr.write('  ' * len(self.stack) +
                                  f'<- {self.symbol(entry.node.addr)} self={entry.node.self_instr}'
                                  f'{tag}  [line {lineno}]\n')


def render_tree(node, ct, out, depth, sort_key, min_count, max_depth):
    children = list(node.children.values())
    if sort_key == 'count':
        children.sort(key=lambda n: n.count, reverse=True)
    elif sort_key == 'self':
        children.sort(key=lambda n: n.self_instr, reverse=True)
    else:
        children.sort(key=lambda n: n.total_instr(), reverse=True)
    for c in children:
        if c.count < min_count:
            continue
        flag = f'  !{c.anomalies} anomalies' if c.anomalies else ''
        print('  ' * depth + f'{ct.symbol(c.addr)}  x{c.count}  '
              f'self={c.self_instr} total={c.total_instr()}{flag}', file=out)
        if max_depth is None or depth + 1 < max_depth:
            render_tree(c, ct, out, depth + 1, sort_key, min_count, max_depth)


def hot_functions(node, agg=None):
    if agg is None:
        agg = collections.defaultdict(lambda: [0, 0])  # addr -> [self_instr, count]
    if node.kind != 'root':
        e = agg[node.addr]
        e[0] += node.self_instr
        e[1] += node.count
    for c in node.children.values():
        hot_functions(c, agg)
    return agg


def dump_report(ct, out, args):
    elapsed = time.monotonic() - ct.start_time
    print(f'=== call tree ({ct.instr_count} instructions, {ct.line_count} trace lines, '
          f'{elapsed:.1f}s wall) ===', file=out)
    if ct.unparsed:
        print(f'({ct.unparsed} unparsed/non-trace lines skipped)', file=out)
    if ct.anomaly_total:
        print(f'{ct.anomaly_total} stack anomalies detected (non-standard returns / '
              f'abandoned frames / resets)', file=out)
    print(file=out)

    print('--- hottest functions (self instructions) ---', file=out)
    agg = hot_functions(ct.root)
    top = sorted(agg.items(), key=lambda kv: kv[1][0], reverse=True)[:args.top]
    for addr, (self_instr, count) in top:
        print(f'  {ct.symbol(addr):40s} self={self_instr:>9d}  calls={count}', file=out)
    print(file=out)

    print('--- call tree ---', file=out)
    render_tree(ct.root, ct, out, 0, args.sort, args.min_calls, args.max_depth)

    if ct.anomaly_log:
        print(file=out)
        print(f'--- last {len(ct.anomaly_log)} anomalies ---', file=out)
        for lineno, addr, mnem, reason in ct.anomaly_log:
            print(f'  line {lineno}: ${addr:04x} {mnem}: {reason}', file=out)

    if ct.track_order:
        print(file=out)
        print('--- tracked memory ranges (--track-mem) ---', file=out)
        for addr in ct.track_order:
            stats = ct.mem_stats.get(addr)
            print(f'{ct.symbol(addr)}:', file=out)
            if not stats or not (stats['reads'] or stats['writes']):
                print('  (not reached)', file=out)
                continue
            for label, counter in (('reads ', stats['reads']), ('writes', stats['writes'])):
                if not counter:
                    print(f'  {label}: (none)', file=out)
                    continue
                parts = []
                for lo, hi, cnt in merge_ranges(counter, args.mem_gap):
                    addrs = f'${lo:04x}' if lo == hi else f'${lo:04x}-${hi:04x}'
                    parts.append(f'{addrs} (x{cnt})')
                print(f'  {label}: ' + ', '.join(parts), file=out)


def parse_addr_list(s, name2addr, opt_name):
    """Parses a comma-separated list of hex/dec addresses or labels-file
    symbol names into an ordered, de-duplicated list of addresses."""
    if not s:
        return []
    addrs = []
    seen = set()
    for tok in (t.strip() for t in s.split(',')):
        if not tok:
            continue
        try:
            addr = int(tok, 0)
        except ValueError:
            if tok not in name2addr:
                sys.exit(f"{opt_name}: unknown symbol '{tok}' "
                         f"(not a number and not in the labels file)")
            addr = name2addr[tok]
        if addr not in seen:
            seen.add(addr)
            addrs.append(addr)
    return addrs


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('trace', nargs='?', default='-',
                     help="trace file to read, or '-' for stdin (default: stdin)")
    default_labels = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'a2.labels')
    ap.add_argument('--labels', default=default_labels,
                     help=f'labels file (default: {default_labels})')
    ap.add_argument('--out', default=None, help='dump file (default: stdout)')
    ap.add_argument('--sort', choices=('total', 'self', 'count'), default='total',
                     help='sort order for the call tree dump')
    ap.add_argument('--min-calls', type=int, default=1,
                     help='prune tree nodes called fewer than this many times')
    ap.add_argument('--max-depth', type=int, default=None, help='max tree depth to print')
    ap.add_argument('--top', type=int, default=25, help='hot-function summary size')
    ap.add_argument('--max-instructions', type=int, default=None,
                     help='stop after this many instructions')
    ap.add_argument('--max-seconds', type=float, default=None,
                     help='stop after this many wall-clock seconds')
    ap.add_argument('--stop-pc', default=None,
                     help='comma-separated hex/dec address(es) or labels-file symbol names; '
                          'stop once one is fetched')
    ap.add_argument('--after-stop-pc', type=int, default=0,
                     help='keep tracing this many more instructions after --stop-pc is hit, '
                          'then stop (default: stop immediately)')
    ap.add_argument('--track-mem', default=None,
                     help='comma-separated hex/dec address(es) or labels-file symbol names of '
                          'function entry points; reports the memory address ranges read/written '
                          'by each one and everything it calls (descendants are folded into the '
                          'selected function\'s totals, not reported separately). Instruction '
                          'fetches, branch/jump targets and stack push/pop are never counted as '
                          'reads or writes -- only actual load/store/RMW data operands are.')
    ap.add_argument('--mem-gap', type=int, default=0,
                     help='merge --track-mem ranges separated by up to this many untouched '
                          'bytes (default: 0, exact contiguous only)')
    ap.add_argument('--status-interval', type=int, default=20000,
                     help='instructions between realtime status updates on stderr (0=off)')
    ap.add_argument('--live', action='store_true',
                     help='print every call/return as it happens, to stderr')
    ap.add_argument('--live-dump', default=None,
                     help='periodically rewrite the call tree to this file while running')
    ap.add_argument('--live-dump-interval', type=float, default=5.0,
                     help='seconds between --live-dump snapshots')
    args = ap.parse_args()

    addr2name, name2addr = load_labels(args.labels)
    if not addr2name:
        print(f'warning: no labels loaded from {args.labels}', file=sys.stderr)

    track_targets = parse_addr_list(args.track_mem, name2addr, '--track-mem')
    ct = CallTree(addr2name, track_targets)
    stop_pcs = set(parse_addr_list(args.stop_pc, name2addr, '--stop-pc'))

    infile = sys.stdin if args.trace == '-' else open(args.trace)
    last_status = time.monotonic()
    last_live_dump = time.monotonic()
    stop_reason = None
    stop_pc_hit_at = None  # instr_count when --stop-pc first matched, for --after-stop-pc

    try:
        for lineno, raw in enumerate(infile, 1):
            ct.feed_line(raw, lineno, live=args.live)

            if stop_pcs and stop_pc_hit_at is None and ct.line_count:
                m = LINE_RE.match(raw)
                if m and int(m.group('addr'), 16) in stop_pcs:
                    stop_pc_hit_at = ct.instr_count
                    hit_addr = int(m.group('addr'), 16)
                    if args.after_stop_pc <= 0:
                        stop_reason = f'reached stop-pc {ct.symbol(hit_addr)}'
                        break
                    sys.stderr.write(f'\nhit stop-pc {ct.symbol(hit_addr)} at instruction '
                                      f'{ct.instr_count}, tracing {args.after_stop_pc} more...\n')
            elif stop_pc_hit_at is not None and ct.instr_count >= stop_pc_hit_at + args.after_stop_pc:
                stop_reason = (f'reached stop-pc {ct.symbol(hit_addr)} '
                                f'+{args.after_stop_pc} instructions')
                break

            if args.max_instructions and ct.instr_count >= args.max_instructions:
                stop_reason = f'reached --max-instructions {args.max_instructions}'
                break

            now = None
            if args.status_interval and ct.instr_count % args.status_interval == 0:
                now = time.monotonic()
                top = ct.stack[-1].node
                sys.stderr.write(f'\r[{ct.instr_count:>10d} instr] depth={len(ct.stack)-1:>3d} '
                                  f'in {ct.symbol(top.addr):30s} anomalies={ct.anomaly_total}   ')
                sys.stderr.flush()
                last_status = now

            if args.max_seconds or args.live_dump:
                now = now or time.monotonic()
                if args.max_seconds and now - ct.start_time >= args.max_seconds:
                    stop_reason = f'reached --max-seconds {args.max_seconds}'
                    break
                if args.live_dump and now - last_live_dump >= args.live_dump_interval:
                    with open(args.live_dump, 'w') as f:
                        dump_report(ct, f, args)
                    last_live_dump = now
    except KeyboardInterrupt:
        stop_reason = 'Ctrl-C'
    finally:
        if infile is not sys.stdin:
            infile.close()
        if args.status_interval:
            sys.stderr.write('\n')
        if stop_reason:
            print(f'--- stopped: {stop_reason} ---', file=sys.stderr)
        out = open(args.out, 'w') if args.out else sys.stdout
        try:
            dump_report(ct, out, args)
        finally:
            if args.out:
                out.close()


if __name__ == '__main__':
    main()

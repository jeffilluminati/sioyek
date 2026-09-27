#!/usr/bin/env python3
"""Summarize xctrace traces, macOS's equivalent of `perf report` / `perf trace`.

usage: xctrace_summary.py trace.trace [--top N] [--thread SUBSTRING]   (Time Profiler samples)
       xctrace_summary.py trace.trace --kernel                         (System Trace: syscalls, VM faults, context switches)
       xctrace_summary.py trace.trace --counters                       (CPU Counters: top-down cycle breakdown)

The first form prints self and inclusive time per function and per thread. The second
aggregates the kernel tables recorded by the System Trace template.
"""
import argparse
import collections
import subprocess
import sys
import xml.etree.ElementTree as ET


def export(trace, schema):
    xpath = '/trace-toc/run[@number="1"]/data/table[@schema="%s"]' % schema
    out = subprocess.run(["xcrun", "xctrace", "export", "--input", trace, "--xpath", xpath],
                         check=True, capture_output=True).stdout
    return ET.fromstring(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--thread", default=None)
    ap.add_argument("--schema", default="time-profile")
    ap.add_argument("--kernel", action="store_true")
    ap.add_argument("--counters", action="store_true")
    args = ap.parse_args()

    if args.kernel:
        return kernel_summary(args)
    if args.counters:
        return counters_summary(args)

    root = export(args.trace, args.schema)
    ids = {}

    def resolve(el):
        if el is None:
            return None
        ref = el.get("ref")
        if ref is not None:
            return ids[ref]
        if el.get("id") is not None:
            ids[el.get("id")] = el
        for child in el:
            resolve(child)
        return el

    self_counts = collections.Counter()
    incl_counts = collections.Counter()
    thread_counts = collections.Counter()
    binary_counts = collections.Counter()
    kernel_samples = 0
    total = 0

    for row in root.iter("row"):
        thread = resolve(row.find("thread"))
        bt = resolve(row.find("backtrace"))
        # weight is in nanoseconds for time-profile rows
        weight_el = resolve(row.find("weight"))
        weight = 1
        if weight_el is not None and weight_el.text:
            try:
                weight = int(weight_el.text) / 1e6  # ms
            except ValueError:
                weight = 1
        thread_name = thread.get("fmt", "?") if thread is not None else "?"
        if args.thread and args.thread not in thread_name:
            continue
        total += weight
        thread_counts[thread_name] += weight
        if bt is None:
            continue
        frames = []
        for fr in bt.findall("frame"):
            fr = resolve(fr)
            name = fr.get("name") or fr.get("addr") or "?"
            binary = resolve(fr.find("binary"))
            bname = binary.get("name") if binary is not None else "?"
            frames.append((name, bname))
        if not frames:
            continue
        leaf_name, leaf_bin = frames[0]
        self_counts[(leaf_name, leaf_bin)] += weight
        binary_counts[leaf_bin] += weight
        if leaf_bin.startswith("kernel"):
            kernel_samples += weight
        for key in set(frames):
            incl_counts[key] += weight

    print("total sampled time: %.1f ms   kernel (self): %.1f ms (%.1f%%)" % (total, kernel_samples, 100.0 * kernel_samples / max(total, 1e-9)))
    print("\n-- by thread")
    for name, c in thread_counts.most_common(8):
        print("%9.1f ms %5.1f%%  %s" % (c, 100.0 * c / total, name[:90]))
    print("\n-- self time by binary")
    for name, c in binary_counts.most_common(10):
        print("%9.1f ms %5.1f%%  %s" % (c, 100.0 * c / total, name))
    print("\n-- self time by function")
    for (name, b), c in self_counts.most_common(args.top):
        print("%9.1f ms %5.1f%%  %-70s %s" % (c, 100.0 * c / total, name[:70], b))
    print("\n-- inclusive time by function")
    for (name, b), c in incl_counts.most_common(args.top):
        print("%9.1f ms %5.1f%%  %-70s %s" % (c, 100.0 * c / total, name[:70], b))


def rows_with_refs(root):
    ids = {}

    def resolve(el):
        if el is None:
            return None
        ref = el.get("ref")
        if ref is not None:
            return ids[ref]
        if el.get("id") is not None:
            ids[el.get("id")] = el
        for child in el:
            resolve(child)
        return el

    for row in root.iter("row"):
        yield row, resolve


def process_name(thread):
    fmt = thread.get("fmt", "") if thread is not None else ""
    return fmt.split("(")[-1].split(",")[0] if "(" in fmt else fmt


def kernel_summary(args):
    target = None

    def interesting(thread):
        name = thread.get("fmt", "") if thread is not None else ""
        return "sioyek" in name

    # system calls: count, wall duration and on-core (kernel CPU) time
    calls = collections.defaultdict(lambda: [0, 0, 0])
    per_thread = collections.defaultdict(lambda: [0, 0])
    root = export(args.trace, "syscall")
    for row, resolve in rows_with_refs(root):
        thread = resolve(row.find("thread"))
        if not interesting(thread):
            continue
        name = resolve(row.find("syscall")).get("fmt", "?")
        durations = [resolve(d) for d in row.findall("duration")]
        dur = int(durations[0].text) if durations else 0
        oncore = resolve(row.find("duration-on-core"))
        cpu = int(oncore.text) if oncore is not None and oncore.text else 0
        c = calls[name]
        c[0] += 1
        c[1] += dur
        c[2] += cpu
        t = per_thread[thread.get("fmt", "?")]
        t[0] += 1
        t[1] += dur
    total_calls = sum(v[0] for v in calls.values())
    total_dur = sum(v[1] for v in calls.values())
    print("-- system calls: %d calls, %.1f ms in kernel" % (total_calls, total_dur / 1e6))
    print("%10s %12s %12s  %s" % ("count", "wall ms", "cpu ms", "syscall"))
    for name, (n, dur, cpu) in sorted(calls.items(), key=lambda kv: -kv[1][1])[:args.top]:
        print("%10d %12.2f %12.2f  %s" % (n, dur / 1e6, cpu / 1e6, name))

    vm = collections.defaultdict(lambda: [0, 0, 0])
    root = export(args.trace, "virtual-memory")
    for row, resolve in rows_with_refs(root):
        thread = resolve(row.find("thread"))
        if not interesting(thread):
            continue
        op = resolve(row.find("vm-op")).get("fmt", "?")
        durations = [resolve(d) for d in row.findall("duration")]
        dur = int(durations[0].text) if durations else 0
        size = resolve(row.find("size-in-bytes"))
        nbytes = int(size.text) if size is not None and size.text else 0
        v = vm[op]
        v[0] += 1
        v[1] += dur
        v[2] += nbytes
    print("\n-- virtual memory events")
    print("%10s %12s %12s  %s" % ("count", "wall ms", "MiB", "operation"))
    for op, (n, dur, nbytes) in sorted(vm.items(), key=lambda kv: -kv[1][1]):
        print("%10d %12.2f %12.1f  %s" % (n, dur / 1e6, nbytes / 2**20, op))

    switches = collections.Counter()
    root = export(args.trace, "context-switch")
    for row, resolve in rows_with_refs(root):
        thread = resolve(row.find("thread"))
        if not interesting(thread):
            continue
        switches[thread.get("fmt", "?")] += 1
    print("\n-- context switches: %d" % sum(switches.values()))
    for name, n in switches.most_common(6):
        print("%10d  %s" % (n, name[:90]))


def counters_summary(args):
    # the CPU Counters template's "bottleneck" mode splits cycles into useful work and
    # instruction delivery (front end) / processing (back end) / discarded (bad speculation) stalls
    root = export(args.trace, "MetricAggregationForProcess")
    sums = collections.defaultdict(float)
    buckets = collections.defaultdict(int)
    for row, resolve in rows_with_refs(root):
        precise = resolve(row.find("boolean"))
        if precise is not None and precise.text == "1":
            continue
        name = resolve(row.find("string")).text
        value = resolve(row.find("fixed-decimal"))
        sums[name] += float(value.text)
        buckets[name] += 1
    avg = {n: sums[n] / buckets[n] for n in sums if n != "cycle"}
    total = sum(avg.values()) or 1.0
    print("-- top-down cycle breakdown (share of the process's busy cycles)")
    for name in sorted(avg, key=lambda n: -avg[n]):
        print("%8.1f %%  %s" % (100.0 * avg[name] / total, name))


if __name__ == "__main__":
    sys.exit(main())

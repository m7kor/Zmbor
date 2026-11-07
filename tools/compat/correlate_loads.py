"""Compare initial ELF coverage gaps with observed SIF writes from one run."""

import argparse
import json
import re
from bisect import bisect_left, bisect_right
from contextlib import nullcontext
from pathlib import Path


def fingerprint(data):
    result = 14695981039346656037
    for byte in data:
        result = ((result ^ byte) * 1099511628211) & 0xffffffffffffffff
    return result


def partition_matches(writes, region, start, end, expected):
    # Keep complete writes that survive in the final image. A partially
    # overwritten write has only a digest for its original full extent.
    retained = []
    covered = 0
    for dest, lo, hi, digest in reversed(writes):
        if dest != region or hi <= start or lo >= end:
            continue
        index = bisect_left(retained, (lo,))
        if index and retained[index - 1][1] > lo:
            index -= 1
        overlap = 0
        for left, right, _ in retained[index:bisect_left(retained, (hi,))]:
            overlap += min(hi, right) - max(lo, left)
        if overlap == min(end, hi) - max(start, lo):
            continue
        if overlap or lo < start or hi > end or not digest:
            return None
        retained.insert(index, (lo, hi, digest))
        covered += hi - lo
        if covered == end - start:
            return all(fingerprint(expected[left - start:right - start]) == int(value, 16)
                       for left, right, value in retained)
    return None


def correlate(analysis, summary, elf=None):
    if not re.search(r'^(Analysis complete|Recompilation completed successfully)$', analysis, re.MULTILINE):
        raise ValueError('expected a successful analyzer or recompiler log')
    mask = re.search(r'^elapsed_ms=\d+ enabled_mask=(\d+)\b', summary, re.MULTILINE)
    counters, marker, ordered = summary.partition('\n-- ordered events ')
    if not mask or not marker:
        raise ValueError('expected a Zmbor trace summary')
    trace_enabled = bool(int(mask[1]) & 16)

    counter = re.search(r'^elf:sif-segment[ \t]+(\d+)[ \t]', counters, re.MULTILINE)
    recorded = int(counter[1]) if counter else 0
    events = re.findall(
        r'^#\d+ t=[0-9.]+ms cat=16 pc=0x[0-9a-f]+ elf:sif-segment'
        r' vaddr=0x[0-9a-f]+ region=(rdram|scratchpad)'
        r' offset=0x([0-9a-f]+) filesz=\d+ memsz=(\d+)(?: fnv1a64=([0-9a-f]{1,16}))?$',
        ordered, re.MULTILINE
    )
    if len(events) > recorded:
        raise ValueError('segment event count exceeds the trace counter')

    writes = [(region, int(offset, 16), int(offset, 16) + int(size), digest)
              for region, offset, size, digest in events if int(size) > 0]
    elf_size = elf.seek(0, 2) if elf is not None else 0
    merged = {}
    for region in ('rdram', 'scratchpad'):
        ranges = sorted((start, end) for dest, start, end, _ in writes if dest == region)
        union = []
        for start, end in ranges:
            if union and start <= union[-1][1]:
                union[-1] = (union[-1][0], max(end, union[-1][1]))
            else:
                union.append((start, end))
        merged[region] = (union, [end for _, end in union])

    gaps = []
    for line in analysis.splitlines():
        if 'bytes covered by initial PT_LOAD; remaining bytes are unverified.' not in line:
            continue
        warning, marker, gap_ranges = line.rpartition(' gaps=')
        if not marker:
            raise ValueError('coverage warning lacks exact gaps; use the current analyzer or recompiler')
        for entry in gap_ranges.split(','):
            extent, _, fingerprint_info = entry.partition('@')
            expected_digest, source_marker, source = fingerprint_info.partition('+')
            match = re.fullmatch(r'(rdram|scratchpad|virtual):0x([0-9a-f]+)-0x([0-9a-f]+)', extent)
            if not match or ('@' in entry and not re.fullmatch(r'[0-9a-f]{1,16}', expected_digest)):
                raise ValueError(f'invalid coverage gap: {entry}')
            if source_marker and not re.fullmatch(r'0x[0-9a-f]+', source):
                raise ValueError(f'invalid gap source offset: {entry}')
            region, start_hex, end_hex = match.groups()
            start, end = int(start_hex, 16), int(end_hex, 16)
            if start >= end:
                raise ValueError(f'empty or reversed coverage gap: {extent}')
            observed = None
            intersections = []
            if trace_enabled and region in merged:
                ranges, ends = merged[region]
                observed = 0
                for i in range(bisect_right(ends, start), len(ranges)):
                    write_start, write_end = ranges[i]
                    if write_start >= end:
                        break
                    lo, hi = max(start, write_start), min(end, write_end)
                    intersections.append(f'0x{lo:x}-0x{hi:x}')
                    observed += hi - lo
            content_matches = None
            if expected_digest and observed == end - start and len(events) == recorded:
                for dest, write_start, write_end, digest in reversed(writes):
                    if dest == region and write_start < end and start < write_end:
                        if digest and (write_start, write_end) == (start, end):
                            content_matches = int(digest, 16) == int(expected_digest, 16)
                        break
            gap_matches = content_matches
            if elf is not None and source_marker and observed == end - start and len(events) == recorded:
                file_offset = int(source, 16)
                if file_offset + end - start > elf_size:
                    raise ValueError(f'ELF bytes disagree with analysis for {extent}')
                elf.seek(file_offset)
                expected = elf.read(end - start)
                if len(expected) != end - start or fingerprint(expected) != int(expected_digest, 16):
                    raise ValueError(f'ELF bytes disagree with analysis for {extent}')
                if gap_matches is None:
                    gap_matches = partition_matches(writes, region, start, end, expected)
            gaps.append({
                'section': warning.strip(),
                'range': extent,
                'bytes': end - start,
                'observed_write_bytes': observed,
                'observed_ranges': intersections,
                'last_sif_write_fingerprint_matches': content_matches,
                'sif_gap_fingerprint_matches': gap_matches,
            })

    return {
        'load_trace_enabled': trace_enabled,
        'segment_writes_counted': recorded,
        'segment_writes_detailed': len(events),
        'segment_details_incomplete': len(events) < recorded,
        'gaps': gaps,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('analysis_log', type=Path)
    parser.add_argument('trace_summary', type=Path)
    parser.add_argument('--elf', type=Path, help='use the analyzed ELF to verify gaps filled by multiple writes')
    args = parser.parse_args()
    try:
        with args.elf.open('rb') if args.elf else nullcontext() as elf:
            report = correlate(args.analysis_log.read_text(encoding='utf-8'),
                               args.trace_summary.read_text(encoding='utf-8'), elf)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()

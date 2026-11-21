#!/usr/bin/env python3
"""Conservative foreground plus non-nested IRQ stack bound for staged builds."""
import argparse
import json
import re
import subprocess
from pathlib import Path

def check(elf, directory, objdump):
    frames = []
    for path in sorted(directory.glob('*.su')):
        for line in path.read_text().splitlines():
            name, size, kind = line.split('\t')
            frames.append({'function': name, 'bytes': int(size), 'kind': kind})
    assembly = subprocess.run([str(objdump), '-d', '--no-show-raw-insn', str(elf)], check=True, text=True, capture_output=True).stdout
    graph, unresolved, indirect = {}, [], []
    current = None
    for line in assembly.splitlines():
        label = re.match(r'^[0-9a-f]+ <([^>]+)>:', line)
        if label:
            current = label[1]; graph.setdefault(current, set()); continue
        instruction = re.match(r'^\s*[0-9a-f]+:\s+(\w+)\s+(.*)', line)
        if not instruction or current is None: continue
        op, args = instruction.groups()
        if op.startswith('jral') or op in ('jr', 'jr5'):
            indirect.append({'function': current, 'instruction': line.strip()})
        if op.startswith('jal') or op in ('j', 'j8'):
            target = re.search(r'<([^>]+)>', args)
            if not target:
                unresolved.append(line.strip())
            elif '+' not in target[1] and target[1] != current:
                graph[current].add(target[1])
    cycles = []
    def visit(node, path):
        if node in path:
            cycles.append(path[path.index(node):] + [node]); return
        if node in visited: return
        for callee in graph.get(node, ()): visit(callee, path+[node])
        visited.add(node)
    visited = set()
    for function in graph: visit(function, [])
    bound = 2*sum(f['bytes'] for f in frames)+72
    result = {'frames': frames, 'assembly_irq_frame_bytes':72,
              'stack_reserved_start':'0xe060', 'stack_top':'0xfb00', 'stack_available_bytes':6816,
              'conservative_bytes':bound,
              'method':'Twice the sum of all static C frames plus 72-byte assembly IRQ frame; bound assumes no recursion and one masked IRQ.',
              'call_graph':{k:sorted(v) for k,v in graph.items()}, 'cycles':cycles,
              'indirect_transfers':indirect, 'unresolved_transfers':unresolved}
    result['accepted'] = bool(frames) and all(f['kind']=='static' for f in frames) and bound<6816 and not cycles and not indirect and not unresolved
    return result

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--elf',type=Path,required=True)
    ap.add_argument('--directory',type=Path,required=True)
    ap.add_argument('--objdump',type=Path,required=True)
    args=ap.parse_args(); result=check(args.elf,args.directory,args.objdump)
    print(json.dumps(result,indent=2)); return 0 if result['accepted'] else 1
if __name__=='__main__': raise SystemExit(main())

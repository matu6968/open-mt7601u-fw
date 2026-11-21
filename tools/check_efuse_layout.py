#!/usr/bin/env python3
"""Check the eFuse milestone's actual ELF/image memory and control-flow layout."""
import argparse
import json
import re
import subprocess
from pathlib import Path

from check_vendor_free import alloc_sections, check_elf, check_image, symbols


def audit_runtime_reservations(sections):
    """Packet evidence is written at boot and must never contain code/data."""
    errors=[]
    for name,address,size,kind in sections:
        reservation=(name=='.packetdiagnostics' and address==0xc900 and size==0xc0 and kind==8)
        if size and address<0xc9c0 and address+size>0xc900 and not reservation:
            errors.append(f'{name} overlaps mutable packet evidence at 0xc900..0xc9bf')
    return errors


def audit_diagnostic_prologue(assembly):
    """Reject a diagnostic trampoline that does not replay the C prologue."""
    functions={}
    current=None
    for line in assembly.splitlines():
        label=re.match(r'^\s*([0-9a-f]+) <([^>]+)>:',line)
        if label:
            current=label[2]; functions[current]=[]
        elif current:
            instruction=re.match(r'^\s*([0-9a-f]+):\s+(\S+)\s*(.*)',line)
            if instruction:
                functions[current].append((int(instruction[1],16),
                    re.sub(r'\s+','',instruction[2]+' '+instruction[3].split('!')[0])))
    if 'independent_usb_prologue_trace' not in functions:
        return []
    body=functions.get('mt7601u_usb_irq_body',[])
    trace=functions['independent_usb_prologue_trace']
    expected=['smw.adm$r6,[$sp],$r10,#0xa','addi$sp,$sp,#-4','ori$r6,$r0,#0']
    extended=len(trace)==14
    tail=(['sethi$r0,#0x410','lwi$r1,[$r0+#0x80]'] if extended else [])
    target=20 if extended else 12
    if (len(body)<3 or [row[1] for row in body[:3]]!=expected or
        [row[0] for row in body[:3]]!=list(range(body[0][0],body[0][0]+12,4)) or
        len(trace) not in (8,14) or [trace[i][1] for i in (1,3,5)]!=expected or
        not all(trace[i][1].startswith('swi.gp'+register+',')
                for i,register in ((0,'$r0'),(2,'$r0'),(4,'$r0'),(6,'$r6'))) or
        extended and ([row[1] for row in body[3:5]]!=tail or
                      [row[1] for row in trace[10:12]]!=tail or
                      trace[7][1]!='movi$r15,#0xcfe0' or trace[8][1]!='lwi$r15,[$r15+#0]' or
                      not trace[9][1].startswith('swi.gp$r15,') or
                      not trace[12][1].startswith('swi.gp$r6,')) or
        trace[-1][1]!=f'j{body[0][0]+target:x}<mt7601u_usb_irq_body+0x{target:x}>'):
        return ['diagnostic USB trampoline differs from exact C prologue']
    return []


def audit_assembly(assembly):
    """Reject resolved ROM targets and straight-line ROM pointer uses.

    SETHI also materializes numeric constants, including the FCE poll budget
    0x100000. Record those for review; a value alone is not an address use.
    This bounded audit complements source/config review, not a CFG proof.
    """
    errors, materializations, values = [], [], {}
    in_rom = lambda value: value is not None and 0x100000 <= value < 0x110000
    for line in assembly.splitlines():
        match = re.match(r"\s*([0-9a-f]+):\s+(\S+)\s*(.*)", line)
        if not match:
            values.clear()
            continue
        pc, opcode, operands = match.groups()
        target = re.search(r"\b([0-9a-f]+)\s+<[^>]+>", operands)
        if target and in_rom(int(target[1], 16)):
            errors.append("IROM instruction reference: " + line.strip())
        registers = re.findall(r"\$[a-z0-9]+", operands)
        base = re.search(r"\[\s*(\$[a-z0-9]+)", operands)
        if base and in_rom(values.get(base[1])):
            errors.append("IROM memory reference: " + line.strip())
        if opcode in ("jr", "jral", "jral5", "ret", "ret5") and registers and in_rom(values.get(registers[0])):
            errors.append("IROM indirect control transfer: " + line.strip())
        immediate = re.search(r"#?(0x[0-9a-f]+|-?\d+)\s*$", operands)
        number = int(immediate[1], 16 if immediate[1].startswith('0x') else 10) if immediate else None
        value = None
        if opcode == "sethi" and number is not None:
            value = number << 12
            if in_rom(value):
                materializations.append({"pc": "0x" + pc, "register": registers[0], "value": hex(value)})
        elif opcode.startswith("movi"):
            value = number
        elif opcode.startswith("mov") and len(registers) > 1:
            value = values.get(registers[1])
        elif opcode in ("ori", "addi") and len(registers) > 1 and number is not None:
            source = values.get(registers[1])
            if source is not None:
                value = source | number if opcode == "ori" else source + number
        if opcode.startswith("jal") or opcode.startswith("lmw"):
            values.clear()
        elif registers and not (opcode.startswith(("swi", "shi", "sbi", "sw", "sh", "sb", "smw", "b", "j", "ret"))):
            values[registers[0]] = value
    return errors, materializations


def check(elf_path, image_path, objdump):
    elf = elf_path.read_bytes()
    errors = check_elf(elf_path) + check_image(image_path)
    sections = alloc_sections(elf)
    errors.extend(audit_runtime_reservations(sections))
    names = dict((name, value) for name, value, _ in symbols(elf))
    for name, address, size, _ in sections:
        if address + size > 0xd000:
            errors.append(f"{name} overlaps log/diagnostic/stack/BootROM reservations")
    if names.get("_stack_top") != 0xfb00:
        errors.append("unexpected acceptance stack top")
    if names.get("__bss_end", 0xffffffff) > 0xd000:
        errors.append("runtime BSS exceeds log-ring boundary")
    if any(0x100000 <= value < 0x110000 for value in names.values()):
        errors.append("ELF contains an IROM-range symbol")
    assembly = subprocess.run([str(objdump), "-d", "--no-show-raw-insn", str(elf_path)],
                              check=True, text=True, capture_output=True).stdout
    assembly_errors, materializations = audit_assembly(assembly)
    errors.extend(assembly_errors)
    errors.extend(audit_diagnostic_prologue(assembly))
    return {"accepted": not errors, "errors": errors,
            "sections": [{"name": n, "address": hex(a), "size": hex(s)} for n, a, s, _ in sections],
            "bss_end": hex(names.get("__bss_end", 0)), "stack_top": hex(names.get("_stack_top", 0)),
            "log_ring": "0xd000..0xe00f", "irq_diagnostics": "0xe020..0xe05f",
            "protected_bootrom_sram": "0xfc00..0xffff",
            "irom_materializations_for_review": materializations,
            "irom_check": "symbols, resolved targets and straight-line ROM pointer uses; source/config review IROM_EP0=0; not a full CFG proof"}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--elf", required=True, type=Path)
    ap.add_argument("--image", required=True, type=Path)
    ap.add_argument("--objdump", required=True, type=Path)
    args = ap.parse_args()
    result = check(args.elf, args.image, args.objdump)
    print(json.dumps(result, indent=2))
    return 0 if result["accepted"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

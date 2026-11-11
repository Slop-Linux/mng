"""Rewrite Darwin-only signal-context accessors and Mach-O assembly symbols to the portable forms of
host_context.h / asm_sym.h, as the M4 Linux port did by hand. Usage: portable.py FILE..."""
import re
import sys

CTX = [
    (r'(\w+)->uc_mcontext->__ss\.__pc', r'\1->UC_PC'),
    (r'(\w+)->uc_mcontext->__ss\.__x', r'\1->UC_X'),
    (r'(\w+)->uc_mcontext->__ss\.__lr', r'\1->UC_LR'),
    (r'(\w+)->uc_mcontext->__ss\.__fp', r'\1->UC_FP'),
    (r'(\w+)->uc_mcontext->__ss\.__sp', r'\1->UC_SP'),
    (r'(\w+)->uc_mcontext->__es\.__far', r'\1->UC_FAR'),
    (r'(\w+)->uc_mcontext->__es\.__esr', r'UC_ESR(\1)'),
    (r'(\w+)->uc_mcontext->__ns\.__v', r'UC_V(\1)'),
    (r'\(\(ucontext_t \*\)(\w+)\)->uc_mcontext->__ss\.__pc', r'((ucontext_t *)\1)->UC_PC'),
    (r'\(\(ucontext_t \*\)(\w+)\)->uc_mcontext->__ss\.__x', r'((ucontext_t *)\1)->UC_X'),
]


def c_file(text):
    out = text
    for pat, rep in CTX:
        out = re.sub(pat, rep, out)
    if out != text and '"host_context.h"' not in out:
        out = add_include(out, '#include "host_context.h"')
    return out


SYMREF = re.compile(r'(?<![\w.])_([A-Za-z]\w*)@PAGEOFF')
SYMPAGE = re.compile(r'(?<![\w.])_([A-Za-z]\w*)@PAGE\b')


def asm_file(text):
    out = SYMREF.sub(r'PAGEOFF(\1)', text)
    out = SYMPAGE.sub(r'PAGE(\1)', out)
    lines = []
    for line in out.split('\n'):
        code, sep, comment = line.partition('//')
        code = re.sub(r'^(\s*\.(?:globl|global|private_extern|weak_definition|p2align\s+\d+,\s*)?\s*)_([A-Za-z]\w*)\b',
                      lambda m: m.group(1) + f'SYM({m.group(2)})', code)
        code = re.sub(r'^_([A-Za-z]\w*):', r'SYM(\1):', code)
        code = re.sub(r'\b(bl|b|blr|br|b\.\w+|cbz|cbnz|tbz|tbnz)(\s+(?:\w+,\s*)*)_([A-Za-z]\w*)\b',
                      r'\1\2SYM(\3)', code)
        code = re.sub(r'\.quad\s+_([A-Za-z]\w*)\b', r'.quad SYM(\1)', code)
        code = code.replace('tpidrro_el0', 'TP_EL0')
        code = code.replace('.cstring', 'CSTRING_SECTION') if code.strip() == '.cstring' else code
        lines.append(code + sep + comment)
    out = '\n'.join(lines)
    if out != text and '"asm_sym.h"' not in out:
        out = '#include "asm_sym.h"\n' + out
    return out


def add_include(text, inc):
    m = list(re.finditer(r'^#include .*$', text, re.M))
    if not m:
        return inc + '\n' + text
    pos = m[0].start()
    return text[:pos] + inc + '\n' + text[pos:]


for path in sys.argv[1:]:
    with open(path) as f:
        text = f.read()
    new = asm_file(text) if path.endswith('.S') else c_file(text)
    if new != text:
        with open(path, 'w') as f:
            f.write(new)
        print('rewrote', path)

"""Compile the proposed gate in isolation using an inspected make dry-run.

The real bootstrap source and canonical build object/dependencies are untouched.
Run under the project's xnu-env.sh so xnu-clang++ and SDK shims are available.
"""
import argparse
from pathlib import Path
import shlex
import subprocess

from prepare_gate import prepare


def compile_gate(source, dryrun, output_dir):
    output_dir.mkdir(parents=True, exist_ok=True)
    output_dir = output_dir.resolve()
    replacement = output_dir / 'bootstrap-gated.cpp'
    replacement.write_text(prepare(source.read_text()))
    lines = [line for line in dryrun.read_text().splitlines() if 'bootstrap.cpp' in line and ' -c ' in line]
    if len(lines) != 1:
        raise ValueError('Expected exactly one bootstrap compile command')
    args = shlex.split(lines[0])
    if args[0] != 'xnu-clang++' or args[-1] != str(source.resolve()):
        raise ValueError('Unexpected dry-run compiler/source')
    args[-1] = str(replacement)
    args[args.index('-o') + 1] = str(output_dir / 'bootstrap-gated.cpo')
    if '-MF' in args:
        args[args.index('-MF') + 1] = str(output_dir / 'bootstrap-gated.cpd')
    # Includes such as meta_features.h are relative to the canonical object dir.
    objdir = source.parent.parent / 'BUILD/obj/DEVELOPMENT_ARM64_BCM2837/libsa/DEVELOPMENT'
    subprocess.run(args, cwd=objdir, check=True)
    print('Gate proposal compiled in isolation; canonical source/object unchanged.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('dryrun', type=Path)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    compile_gate(args.source, args.dryrun, args.output_dir)

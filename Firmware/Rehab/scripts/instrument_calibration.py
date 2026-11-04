#!/usr/bin/env python3
"""Generate a V1.3-only observation copy; never rewrite the native motor source.

Only marked insertions are allowed. Removing them must recover every original
byte, including line endings. An unexpected or non-unique anchor fails closed.
"""
import argparse
from pathlib import Path
import re

PREFIX = b'// V13_CAL_CAPTURE_BEGIN '
BLOCK = re.compile(rb'// V13_CAL_CAPTURE_BEGIN ([a-z_]+)\r?\n.*?// V13_CAL_CAPTURE_END \1\r?\n', re.S)


def strip_instrumentation(source):
    return BLOCK.sub(b'', source)


def instrument(source):
    if b'V13_CAL_CAPTURE_' in source:
        raise ValueError('Input already contains diagnostic instrumentation')
    newline = b'\r\n' if b'\r\n' in source else b'\n'
    output = source
    inserted = 0

    def after(anchor, name, lines):
        nonlocal output, inserted
        needle = anchor.encode().replace(b'\n', newline)
        if output.count(needle) != 1:
            raise ValueError('Missing/non-unique native anchor: ' + name)
        block = ('// V13_CAL_CAPTURE_BEGIN ' + name + '\n' + lines + '\n'
                 '// V13_CAL_CAPTURE_END ' + name + '\n').encode().replace(b'\n', newline)
        output = output.replace(needle, needle + block, 1)
        inserted += 1

    after('#include <algorithm>\n', 'include',
          '#include <Rehab/platform/calibration_capture.hpp>')
    after('            test_mod_ = test_voltage_ * vfactor;\n', 'resistance_measure',
          '            if (Ialpha_beta.has_value()) {\n'
          '                rehab::capture::model_measure(1, Ialpha_beta->first, Ialpha_beta->second,\n'
          '                    test_voltage_, *test_mod_ * actual_current_, true);\n'
          '            } else {\n'
          '                rehab::capture::model_measure(1, NAN, NAN, test_voltage_, NAN, false);\n'
          '            }')
    after('            *ibus = *test_mod_ * actual_current_;\n', 'resistance_output',
          '            rehab::capture::model_output(1, test_voltage_, **ibus, output_timestamp, true);')
    after('        last_input_timestamp_ = input_timestamp;\n', 'inductance_measure',
          '        rehab::capture::model_measure(2, Ialpha_beta->first, Ialpha_beta->second,\n'
          '            test_voltage_, 0.0f, true);')
    after('        *ibus = 0.0f;\n', 'inductance_output',
          '        rehab::capture::model_output(2, test_voltage_, **ibus, output_timestamp, true);')
    if inserted != 5 or len(BLOCK.findall(output)) != 5 or strip_instrumentation(output) != source:
        raise ValueError('Instrumentation does not preserve native source bytes')
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    source_path, output_path = Path(args.input), Path(args.output)
    if source_path.resolve() == output_path.resolve():
        raise ValueError('Output must not overwrite original source')
    result = instrument(source_path.read_bytes())
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(result)


if __name__ == '__main__':
    main()

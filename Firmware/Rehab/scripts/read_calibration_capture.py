"""Read/export a frozen V1.3 capture from odrivetool. Never arms or moves a motor."""
import csv
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import struct

NAMES = ('sequence timestamp phase flags adc2 adc3 adc2_sr adc3_sr '
         'ialpha ibeta test_voltage estimated_ibus vbus dc_a dc_b dc_c inverse_gain '
         'trip runtime_ibus motor_error_lo motor_error_hi system_error '
         'bdtr_sample bdtr_after armed_state output_timestamp reserved26 reserved27').split()


def decode(words):
    assert len(words) == 28
    row = dict(zip(NAMES, words))
    for i in range(8, 19):
        value = struct.unpack('<f', struct.pack('<I', words[i]))[0]
        row[NAMES[i]] = value if math.isfinite(value) else None
    return row


def read_capture(dev):
    assert dev.rehab.board_revision == 0x1301, 'Wrong board'
    assert dev.axis0.current_state == 1 and not dev.axis0.motor.is_armed, 'Read after motor is idle'
    get = dev.rehab.cal_capture_word
    header = [int(get(i)) for i in range(9)]
    assert header[0] == 1 and header[6:8] == [64, 28], 'Unsupported capture format'
    assert header[1] == 2, 'Capture not frozen; do not automatically retry calibration'
    assert 0 < header[3] <= 64, 'Invalid record count'
    words = [[int(get(16 + row*28 + col)) for col in range(28)] for row in range(header[3])]
    assert int(get(1)) == 2 and int(get(2)) == header[2], 'Capture changed during read'
    rows = [decode(w) for w in words]
    report = {'format': 1, 'header': header, 'raw_words': words, 'records': rows,
              'reason': {1: 'negative_model_current', 2: 'native_protection', 3: 'calibration_ended'}.get(header[4]),
              'note': 'Commanded test voltage and computed bus current, not independent physical measurements. Missing model values are null.'}
    folder = Path(__file__).resolve().parents[2] / 'build/rehab/calibration_captures'
    folder.mkdir(parents=True, exist_ok=True)
    stem = 'capture-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S-%fZ')
    path = folder / (stem + '.json')
    path.write_text(json.dumps(report, indent=2, allow_nan=False), encoding='utf-8')
    with (folder / (stem + '.csv')).open('w', newline='', encoding='utf-8-sig') as file:
        writer = csv.DictWriter(file, fieldnames=NAMES)
        writer.writeheader(); writer.writerows(rows)
    print('Frozen:', report['reason'], 'generation:', header[2], 'records:', len(rows))
    print('seq phase flags ADC2 ADC3 Ialpha Ibeta Vtest Ibus_model')
    for row in rows[-12:]:
        print(*(row[key] for key in ('sequence','phase','flags','adc2','adc3','ialpha','ibeta','test_voltage','estimated_ibus')))
    print('JSON:', path)
    print('CSV:', folder / (stem + '.csv'))
    return report


if __name__ == '__main__':
    if 'dev0' not in globals():
        raise RuntimeError('Run inside odrivetool with runpy.run_path(..., init_globals={"dev0": dev0}, run_name="__main__")')
    read_capture(dev0)

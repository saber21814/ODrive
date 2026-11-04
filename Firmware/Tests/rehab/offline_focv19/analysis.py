"""Pure-host FOCV1.9 audit models. No device, network, ODrive or serial imports.

Candidate debounce/filter policies here are counterfactual experiments, NEVER
firmware replacements. All thresholds remain -0.01 A in these comparisons.
"""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import statistics
import struct

BASELINE = '3abc1e3290acc4c6054caa2d7b6a696118a0fd75'
HERE = Path(__file__).resolve().parent
FIXTURE = HERE / 'fixtures/capture.json'
FIXTURE_SHA = 'bbafcc3773ce1c28e47429bfd32f01b4eeed6b9603ed3aefd2126f8585d53f46'
LIMIT = -0.01
DT = 21000 / 168000000
K = 3.3 / 4096 / (80 * 0.0005)


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def adc_abc(b, c, offsets=(0.0, 0.0, 0.0)):
    """Nominal 3.3V conversion then native offset subtraction (double model)."""
    ib, ic = (b-2048)*K, (c-2048)*K
    return -ib-ic-offsets[0], ib-offsets[1], ic-offsets[2]


def clarke(abc):
    return abc[0], (abc[1]-abc[2])/math.sqrt(3)


def bus_estimate(voltage, alpha, vbus):
    return voltage / ((2/3)*vbus) * alpha


def bus_estimate_f32(voltage, alpha, vbus):
    # Matches motor.cpp evaluation order: vfactor, test_mod, multiplication.
    factor = f32(1 / f32(f32(2/3)*f32(vbus)))
    return f32(f32(f32(voltage)*factor)*f32(alpha))


def original_trip(x, limit=LIMIT):
    return x < limit  # Raw comparison, including original NaN comparison semantics.


def apply_native_brake(ibus, vbus, enabled=False, resistance=.47, allowed=0.0):
    """Simplified low_level.cpp branch, voltage ramp OFF; not hardware behavior."""
    if not enabled:
        return ibus
    if resistance <= 0 or vbus <= 0:
        raise ValueError('invalid model input')
    duty = min(.95, max(0.0, (-ibus-allowed)*resistance/vbus))
    return ibus + duty*vbus/resistance


def policy_events(values, policy='raw', count=2, alpha=.25):
    """Return each triggering index; first index is the proposed trip instant.

    EMA starts at zero. No NaN substitution is used; nonfinite scenario inputs
    are rejected because these experiments are not a replacement fault policy.
    """
    if count < 1 or not 0 < alpha <= 1 or policy not in ('raw','consecutive','ema'):
        raise ValueError('policy')
    hits, streak, filtered = [], 0, 0.0
    for index, value in enumerate(values):
        if not math.isfinite(value):
            raise ValueError('nonfinite simulation input')
        if policy == 'raw':
            fire = original_trip(value)
        elif policy == 'consecutive':
            streak = streak+1 if original_trip(value) else 0
            fire = streak >= count
        else:
            filtered += alpha*(value-filtered)
            fire = original_trip(filtered)
        if fire:
            hits.append(index)
    return hits


POLICIES = {'raw': {'policy':'raw'}, 'two_samples': {'policy':'consecutive','count':2},
            'three_samples': {'policy':'consecutive','count':3},
            'ema_025': {'policy':'ema','alpha':.25}}


def load_capture(path=FIXTURE):
    data = json.loads(Path(path).read_text(encoding='utf-8'))
    if path == FIXTURE and hashlib.sha256(Path(path).read_bytes()).hexdigest() != FIXTURE_SHA:
        raise ValueError('fixture hash mismatch')
    return data


def scenarios(records):
    measured = [r['estimated_ibus'] for r in records]
    prefix = measured[:-1]
    # Post-trigger samples are unavailable. Every extension below is synthetic.
    return {
        'captured_window_right_censored': measured,
        'synthetic_isolated_noise_then_recovery': prefix+[measured[-1]]+[-.002]*40,
        'synthetic_sustained_minus_020': prefix+[-.020]*40,
        'synthetic_sustained_minus_0101': prefix+[-.0101]*60,
        'synthetic_sustained_minus_100': prefix+[-.100]*40,
        'synthetic_real_single_pulse': prefix+[-.020]+[0.0]*40,
        'synthetic_repeated_real_pulses': prefix+[-.020,0.0]*20,
    }


def analyze(path=FIXTURE):
    data=load_capture(path); rows=data['records']; last=rows[-1]; prev=rows[-2]
    actual_dts=sorted({(b['timestamp']-a['timestamp']) & 0xffffffff for a,b in zip(rows,rows[1:])})
    reconstructed=[clarke(adc_abc(r['adc2'],r['adc3'],(r['dc_a'],r['dc_b'],r['dc_c']))) for r in rows]
    comparison=[]
    for name, values in scenarios(rows).items():
        raw=policy_events(values); onset=raw[0] if raw else None
        for policy, args in POLICIES.items():
            events=policy_events(values,**args); first=events[0] if events else None
            comparison.append({'scenario':name,'policy':policy,'first_index':first,
                               'delay_us_vs_raw':None if first is None or onset is None else (first-onset)*DT*1e6,
                               'result':'triggered' if events else ('unknown_after_capture' if name.endswith('censored') else 'not_detected'),
                               'note':'Only synthetic noise has a false-trigger label; a real short pulse must not be called noise.'})
    da,db=last['adc2']-prev['adc2'],last['adc3']-prev['adc3']
    delta_dc_a=last['dc_a']-prev['dc_a']
    delta_dc_beta=((last['dc_b']-prev['dc_b'])-(last['dc_c']-prev['dc_c']))/math.sqrt(3)
    return {'baseline':BASELINE,'fixture_sha256':hashlib.sha256(Path(path).read_bytes()).hexdigest(),
            'sample_count':len(rows),'sample_ticks':actual_dts,'sample_period_us':DT*1e6,
            'pre63_alpha_mean':statistics.mean(r['ialpha'] for r in rows[:-1]),
            'pre63_alpha_range':[min(r['ialpha'] for r in rows[:-1]),max(r['ialpha'] for r in rows[:-1])],
            'amps_per_count':K,'delta_adc':[da,db],
            'delta_alpha_from_adc':-(da+db)*K,'delta_alpha_offset_term':-delta_dc_a,
            'delta_alpha_predicted':-(da+db)*K-delta_dc_a,
            'delta_beta_predicted':(da-db)*K/math.sqrt(3)-delta_dc_beta,
            'alpha_reconstruction_max_error':max(abs(x[0]-r['ialpha']) for x,r in zip(reconstructed,rows)),
            'beta_reconstruction_max_error':max(abs(x[1]-r['ibeta']) for x,r in zip(reconstructed,rows)),
            'last_estimate_f32':bus_estimate_f32(last['test_voltage'],last['ialpha'],last['vbus']),
            'last_estimate_at_nominal24V':bus_estimate(last['test_voltage'],last['ialpha'],24),
            'last':last, 'comparisons':comparison,
            'limits':['The capture contains no physical current-direction reference.',
                      'No post-trigger samples; filter false-positive rates cannot be inferred.',
                      'Brake-enable, max_regen_current and effective native limits are not recorded.',
                      'ADC SR / PWM MOE are software snapshots, not analog waveform evidence.']}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture',type=Path,default=FIXTURE)
    parser.add_argument('--output',type=Path,default=HERE/'results')
    args=parser.parse_args(); result=analyze(args.capture); args.output.mkdir(parents=True,exist_ok=True)
    (args.output/'analysis.json').write_text(json.dumps(result,indent=2,ensure_ascii=False,allow_nan=False),encoding='utf-8')
    with (args.output/'policy_comparison.csv').open('w',newline='',encoding='utf-8-sig') as file:
        writer=csv.DictWriter(file,fieldnames=list(result['comparisons'][0]));writer.writeheader();writer.writerows(result['comparisons'])
    print('OFFLINE ONLY:',BASELINE)
    print('Samples:',result['sample_count'],'A/count:',K,'last Ibus:',result['last_estimate_f32'])
    for row in result['comparisons']:
        print(row['scenario'],row['policy'],row['result'],'delay_us=',row['delay_us_vs_raw'])
    print('Outputs:',args.output)


if __name__=='__main__': main()

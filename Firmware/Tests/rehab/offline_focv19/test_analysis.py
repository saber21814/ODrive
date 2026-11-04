"""Run: python -B -m unittest discover -s Firmware/Tests/rehab/offline_focv19 -v"""
import hashlib
import json
import math
from pathlib import Path
import unittest
import analysis as a


class CapturedEvidence(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data=a.load_capture(); cls.rows=cls.data['records']; cls.result=a.analyze()

    def test_exact_fixture_and_single_crossing(self):
        self.assertEqual(len(self.rows),64)
        self.assertEqual(a.policy_events([r['estimated_ibus'] for r in self.rows]),[63])
        self.assertEqual(self.data['header'][4:6],[1,63])

    def test_decoded_records_match_raw_words(self):
        for raw,r in zip(self.data['raw_words'],self.rows):
            self.assertEqual(raw[4],r['adc2']); self.assertEqual(raw[5],r['adc3'])
            self.assertEqual(a.struct.unpack('<f',a.struct.pack('<I',raw[8]))[0],r['ialpha'])
            self.assertEqual(a.struct.unpack('<f',a.struct.pack('<I',raw[11]))[0],r['estimated_ibus'])

    def test_control_clock_and_pretrigger_negative_bias(self):
        self.assertEqual(self.result['sample_ticks'],[21000])
        self.assertAlmostEqual(a.DT,125e-6)
        self.assertAlmostEqual(self.result['pre63_alpha_mean'],-.08840858486909715)
        self.assertTrue(all(r['ialpha']<0 for r in self.rows[:-1]))

    def test_adc_gain_and_all64_clarke_samples(self):
        self.assertAlmostEqual(a.K,.0201416015625)
        self.assertLess(self.result['alpha_reconstruction_max_error'],3e-7)
        self.assertLess(self.result['beta_reconstruction_max_error'],3e-7)

    def test_common_mode_jump_and_offset_change(self):
        self.assertEqual(self.result['delta_adc'],[12,10])
        self.assertAlmostEqual(self.result['delta_alpha_from_adc'],-.443115234375)
        r,p=self.rows[-1],self.rows[-2]
        self.assertAlmostEqual(self.result['delta_alpha_predicted'],r['ialpha']-p['ialpha'],places=6)
        self.assertAlmostEqual(self.result['delta_beta_predicted'],r['ibeta']-p['ibeta'],places=6)
        self.assertLess(abs(self.result['delta_alpha_offset_term']),.0001)

    def test_bus_f32_replays_all_records(self):
        for r in self.rows:
            self.assertAlmostEqual(a.bus_estimate_f32(r['test_voltage'],r['ialpha'],r['vbus']),r['estimated_ibus'],places=8)
        self.assertAlmostEqual(self.result['last_estimate_f32'],-.010212470777332783)
        self.assertNotAlmostEqual(self.rows[-1]['vbus'],24,places=1)

    def test_error_and_pwm_transition_are_in_last_record(self):
        self.assertTrue(all(r['motor_error_lo']==0 and r['system_error']==0 for r in self.rows[:-1]))
        last=self.rows[-1]
        self.assertEqual(last['system_error'],8)
        self.assertEqual(last['motor_error_lo'],0x1000000)
        self.assertTrue(last['bdtr_sample'] & 0x8000)
        self.assertFalse(last['bdtr_after'] & 0xC000)
        self.assertTrue(all(r['flags']==63 and r['phase']==1 for r in self.rows))

    def test_no_claim_of_post_trigger_recovery(self):
        values=[r['estimated_ibus'] for r in self.rows]
        self.assertFalse(a.policy_events(values,'consecutive',count=2))
        self.assertFalse(a.policy_events(values,'ema'))
        self.assertEqual(len(values),64)  # No fabricated post-trigger samples.


class PolicyTradeoffs(unittest.TestCase):
    def test_strict_threshold(self):
        self.assertFalse(a.original_trip(-.01))
        self.assertTrue(a.original_trip(-.010001))

    def test_common_vs_differential_and_bc_swap(self):
        zero=a.clarke(a.adc_abc(2048,2048))
        common=a.clarke(a.adc_abc(2059,2059))
        diff=a.clarke(a.adc_abc(2059,2037))
        self.assertEqual(zero,(0,0))
        self.assertAlmostEqual(common[0],-22*a.K); self.assertEqual(common[1],0)
        self.assertEqual(diff[0],0); self.assertGreater(diff[1],0)
        x=a.clarke(a.adc_abc(2051,2073)); y=a.clarke(a.adc_abc(2073,2051))
        self.assertEqual(x[0],y[0]); self.assertEqual(x[1],-y[1])

    def test_consecutive_delays_true_sustained_event(self):
        values=[0]*20+[-.02]*40
        self.assertEqual(a.policy_events(values)[0],20)
        self.assertEqual(a.policy_events(values,'consecutive',2)[0],21)
        self.assertEqual(a.policy_events(values,'consecutive',3)[0],22)
        self.assertEqual(a.policy_events(values,'ema',alpha=.25)[0],22)

    def test_short_real_event_can_be_missed(self):
        values=[0]*20+[-.02]+[0]*30
        self.assertEqual(a.policy_events(values),[20])
        self.assertFalse(a.policy_events(values,'consecutive',2))
        self.assertFalse(a.policy_events(values,'ema',alpha=.25))

    def test_near_threshold_filter_delay_not_constant(self):
        severe=[0]*20+[-.1]*40; near=[0]*20+[-.0101]*60
        self.assertEqual(a.policy_events(severe,'ema')[0],20)
        self.assertGreater(a.policy_events(near,'ema')[0]-20,10)

    def test_consecutive_reset_can_miss_repeated_real_pulses(self):
        values=[-.02,0]*20
        self.assertTrue(a.policy_events(values))
        self.assertFalse(a.policy_events(values,'consecutive',2))

    def test_finite_domain_is_explicit(self):
        for bad in [float('nan'),float('inf'),-float('inf')]:
            with self.assertRaises(ValueError): a.policy_events([bad],'ema')

    def test_brake_input_vs_net_input_not_conflated(self):
        value=-.010212470777332783
        self.assertTrue(a.original_trip(a.apply_native_brake(value,23.7115)))
        net=a.apply_native_brake(value,23.7115,enabled=True,allowed=0)
        self.assertAlmostEqual(net,0);self.assertFalse(a.original_trip(net))

    def test_sample_time_is_not_entire_settling_window(self):
        self.assertAlmostEqual(3/21e6,142.857142857e-9)
        self.assertAlmostEqual((3+12)/21e6,714.2857142857e-9)
        self.assertAlmostEqual(1622/168e6,9.65476190476e-6)
        self.assertAlmostEqual(10500/168e6,62.5e-6)

    def test_ov_proposal_is_explicit_profile_only(self):
        # This is a proposal model, NOT firmware code or device configuration.
        def proposal(board,voltage,explicit_test=False,saved=None,profile='peripherals',test_voltage=24):
            selected=board=='v13' and explicit_test and profile=='peripherals' and test_voltage==24
            default=28.0 if selected else 1.07*voltage
            return default if saved is None else saved
        self.assertEqual(proposal('v13',0),0)
        self.assertEqual(proposal('v13',0,True),28)
        self.assertEqual(proposal('v13',48),51.36)
        self.assertEqual(proposal('v3',56,True),59.92)
        self.assertEqual(proposal('v13',0,True,saved=0),0) # default does not silently rewrite saved data
        self.assertEqual(proposal('v13',0,True,profile='full'),0)
        self.assertEqual(proposal('v13',48,True,test_voltage=48),51.36)
        self.assertEqual(proposal('v12',0,True),0)


if __name__=='__main__': unittest.main()

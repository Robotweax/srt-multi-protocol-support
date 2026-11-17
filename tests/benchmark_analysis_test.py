"""Verify the performance gate cannot turn short/noisy measurements into a pass."""
import math
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'benchmarks'))
from compare import interval, summarize


class AnalysisTests(unittest.TestCase):
    def setUp(self):
        self.args = SimpleNamespace(min_seconds=5, max_baseline_cv=.05,
                                    throughput_budget=2, cpu_budget=5)

    def rows(self, throughput=1, cpu=1, count=8, seconds=10, noisy=False):
        rows = []
        for provider in ['robotweax', 'haivision']:
            for block in range(count):
                base = 1000 * (1 + (.3 if block % 2 else -.3)) if noisy else 1000
                for path in ['direct', 'module', 'facade']:
                    wrapped = path != 'direct'
                    rows.append(dict(provider=provider, path=path, block=block, bytes=1000000,
                        mbps=base*(throughput if wrapped else 1), seconds=seconds,
                        cpu_seconds=8*(cpu if wrapped else 1), application_cpu_seconds=2))
        return rows

    def decisions(self, rows):
        report = summarize(rows, self.args)
        return [p['decision'] for v in report.values() for p in v['paths'].values()]

    def test_equivalence_and_significant_regressions(self):
        self.assertEqual(self.decisions(self.rows()), ['pass_measured_profile']*4)
        self.assertEqual(self.decisions(self.rows(throughput=.9)), ['regression']*4)
        self.assertEqual(self.decisions(self.rows(cpu=1.1)), ['regression']*4)

    def test_no_false_pass_from_inadequate_controls(self):
        for rows in [self.rows(count=1), self.rows(seconds=.5), self.rows(noisy=True)]:
            self.assertEqual(self.decisions(rows), ['inconclusive_controls']*4)

    def test_paired_log_ratios_and_uncertainty(self):
        result = interval([.9, 1.1])
        self.assertAlmostEqual(result['ratio'], math.sqrt(.99))
        self.assertLess(result['low95'], .98)
        self.assertGreater(result['high95'], 1)


if __name__ == '__main__':
    unittest.main()

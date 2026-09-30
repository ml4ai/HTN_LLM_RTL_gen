"""Unit tests for the harness's statistics and extraction (no tools needed).

    python eval/test_eval.py
"""

import math
import os
import random
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import passk  # noqa: E402
from benchmarks import RTLLM2, VerilogEvalV2, has_module, top_modules  # noqa: E402


class PassAtK(unittest.TestCase):
    def test_matches_the_binomial_definition(self):
        for n in (1, 5, 20):
            for c in range(n + 1):
                for k in range(1, n + 1):
                    exact = 1 - math.comb(n - c, k) / math.comb(n, k)
                    self.assertAlmostEqual(passk.pass_at_k(n, c, k), exact, places=12)

    def test_pass_at_1_is_the_success_rate(self):
        self.assertAlmostEqual(passk.pass_at_k(20, 7, 1), 7 / 20)

    def test_never_decreases_with_k(self):
        rng = random.Random(0)
        for _ in range(200):
            counts = [rng.randint(0, 20) for _ in range(30)]
            lad = passk.ladder(counts, 20, [1, 5, 10])
            self.assertLessEqual(lad[1], lad[5] + 1e-12)
            self.assertLessEqual(lad[5], lad[10] + 1e-12)

    def test_rejects_too_few_samples(self):
        with self.assertRaises(ValueError):
            passk.pass_at_k(5, 1, 10)


class Split(unittest.TestCase):
    def test_terms_sum_to_the_difference(self):
        rng = random.Random(1)
        for _ in range(10000):
            sp, sd, qp, qd = (rng.random() for _ in range(4))
            st, ct = passk.split(sp, qp, sd, qd)
            self.assertAlmostEqual(st + ct, sp * qp - sd * qd, places=12)

    def test_undefined_conditional_rate_goes_to_syntax(self):
        st, ct = passk.split(0.5, 0.4, 0.0, None)
        self.assertAlmostEqual(st, 0.2)
        self.assertEqual(ct, 0.0)
        self.assertEqual(passk.split(0, None, 0, None), (0.0, 0.0))


class McNemar(unittest.TestCase):
    def test_symmetric_and_bounded(self):
        self.assertEqual(passk.mcnemar_exact(0, 0), 1.0)
        self.assertAlmostEqual(passk.mcnemar_exact(3, 7), passk.mcnemar_exact(7, 3))
        self.assertAlmostEqual(passk.mcnemar_exact(0, 6), 2 / 64)


class Extraction(unittest.TestCase):
    CODE = "module TopModule(input a, output b);\n  assign b = a;\nendmodule"

    def test_verilog_eval_begin_done(self):
        out = VerilogEvalV2.extract("[BEGIN]\n" + self.CODE + "\n[DONE]\n")
        self.assertIn("assign b = a;", out)
        self.assertNotIn("[BEGIN]", out)

    def test_verilog_eval_backtick_fallback(self):
        out = VerilogEvalV2.extract("Here it is:\n```verilog\n" + self.CODE + "\n```\nDone.")
        self.assertIn("assign b = a;", out)
        self.assertNotIn("Here it is", out)

    def test_verilog_eval_unterminated_begin_yields_no_code(self):
        self.assertFalse(has_module(VerilogEvalV2.extract("[BEGIN]\n" + self.CODE)))

    def test_rtllm_prefers_the_block_defining_the_design(self):
        reply = ("```verilog\nmodule fsm(input a);\nendmodule\n```\n"
                 "And a testbench:\n```verilog\nmodule tb;\n  fsm dut(.a(1'b0));\nendmodule\n```\n")
        out = RTLLM2.extract(reply, "fsm")
        self.assertIn("module fsm", out)
        self.assertNotIn("module tb", out)

    def test_rtllm_accepts_systemverilog_fences_and_bare_code(self):
        self.assertIn("module fsm", RTLLM2.extract("```systemverilog\nmodule fsm;\nendmodule\n```", "fsm"))
        self.assertIn("endmodule", RTLLM2.extract("Sure.\nmodule fsm;\nendmodule\nThanks.", "fsm"))
        self.assertEqual(RTLLM2.extract("I cannot help with that.", "fsm"), "")

    def test_top_modules(self):
        src = "module leaf(input a);\nendmodule\nmodule top;\n  leaf u0(.a(1'b1));\nendmodule\n"
        self.assertEqual(top_modules(src), ["top"])


if __name__ == "__main__":
    unittest.main(verbosity=1)

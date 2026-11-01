# SPDX-License-Identifier: Apache-2.0
"""Tests of abm-gen on the miniature BioDynaMo in this folder."""
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools", "abm-gen"))
import abm_gen  # noqa: E402

INCLUDES = ["-I" + os.path.join(ROOT, "include"), "-I" + HERE]
BDM = os.path.join(HERE, "bdm_like.h")
MODEL = os.path.join(HERE, "my_model.h")
SIDE = os.path.join(HERE, "units.yaml")


def generate(headers, side=None, includes=None):
    out = os.path.join(tempfile.mkdtemp(), "generated.h")
    argv = ["--out", out]
    if side:
        argv += ["--side", side]
    for h in includes or []:
        argv += ["--include", h]
    rc = abm_gen.main(argv + headers + ["--"] + INCLUDES)
    with open(out) if rc == 0 else open(os.devnull) as f:
        return rc, f.read()


def generator():
    flags = ["-std=c++17"] + ["-isystem" + d for d in abm_gen.system_include_dirs(
        os.environ.get("CXX", "c++"))] + INCLUDES
    return abm_gen.Generator(flags)


def header(text):
    path = os.path.join(tempfile.mkdtemp(), "case.h")
    with open(path, "w") as f:
        f.write('#include "bdm_like.h"\n' + text)
    return path


def section(text, type_name):
    start = text.index(f"struct TypeTraits<{type_name}>")
    return text[start:text.index("};", start)]


class Generated(unittest.TestCase):
    def test_matches_the_expected_header(self):
        rc, text = generate([BDM, MODEL], SIDE, ["bdm_like.h", "my_model.h", "bdm_adapter.h"])
        self.assertEqual(rc, 0)
        with open(os.path.join(HERE, "expected_generated.txt")) as f:
            self.assertEqual(text, f.read())

    def test_a_models_base_keeps_its_own_version(self):
        rc, text = generate([MODEL])  # bdm::Cell is defined in another header
        self.assertEqual(rc, 0)
        self.assertIn("kVersion = 4;", section(text, "bdm::Cell"))
        self.assertIn("using Base = bdm::Agent;", section(text, "bdm::Cell"))
        self.assertIn("using Base = void;", section(text, "bdm::Agent"))
        self.assertNotIn("bdm::GrowthDivision", text)  # not in the model's chain

    def test_transient_and_static_members_are_left_out(self):
        rc, text = generate([BDM, MODEL])
        self.assertEqual(rc, 0)
        for member in ("lock_", "is_static_", "cached_pressure_", "instances_", "kDimensions"):
            self.assertNotIn("::" + member, text)
        self.assertIn('v(Field("box_idx"), &bdm::Agent::box_idx_);', text)


class Errors(unittest.TestCase):
    def setUp(self):
        self.gen = generator()
        self.gen.parse(BDM)

    def side_error(self, side):
        with self.assertRaises(abm_gen.GeneratorError) as e:
            self.gen.apply_side_file(side)
        return str(e.exception)

    def test_side_file_mistakes(self):
        self.assertIn("no described type bdm::Missing", self.side_error({"bdm::Missing": {}}))
        self.assertIn("has no field radius", self.side_error({"bdm::Cell": {"radius": {}}}))
        self.assertIn("unknown keys ['units']",
                      self.side_error({"bdm::Cell": {"volume": {"units": "um3"}}}))
        self.assertIn("unknown roles ['send']",
                      self.side_error({"bdm::Cell": {"volume": {"roles": ["send"]}}}))

    def test_roles_can_be_none(self):
        self.gen.apply_side_file({"bdm::Cell": {"volume": {"roles": []}}})
        text = self.gen.render(["bdm_like.h"], ["bdm_like.h"])
        self.assertIn('Field("volume").Roles(Role::kNone)', text)

    def test_a_wrong_base_in_the_macro(self):
        with self.assertRaises(abm_gen.GeneratorError) as e:
            generator().parse(header(
                "namespace bdm {\nclass X : public Agent {\n  BDM_AGENT_HEADER(X, Cell, 1);\n};\n}\n"))
        self.assertIn("the macro names base Cell, but the class derives from bdm::Agent",
                      str(e.exception))

    def test_a_base_in_the_macro_without_a_base_class(self):
        with self.assertRaises(abm_gen.GeneratorError) as e:
            generator().parse(header("namespace bdm {\nclass Y {\n  BDM_AGENT_HEADER(Y, Agent, 1);\n};\n}\n"))
        self.assertIn("but the class has no base class", str(e.exception))

    def test_parse_errors_are_reported(self):
        with self.assertRaises(abm_gen.GeneratorError) as e:
            generator().parse(header("class Z : public bdm::Agent { int x_ };\n"))
        self.assertIn("case.h", str(e.exception))

    def test_class_templates_are_refused(self):
        with self.assertRaises(abm_gen.GeneratorError) as e:
            generator().parse(header("template <class T>\nclass W : public bdm::Agent {\n"
                                     "  BDM_AGENT_HEADER(W, Agent, 1);\n};\n"))
        self.assertIn("class templates are not supported", str(e.exception))


if __name__ == "__main__":
    unittest.main()

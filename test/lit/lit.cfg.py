# lit configuration for goc's LLVM passes and goc-llc. Run with
# test/lit/run.sh (it finds lit and FileCheck), or
#   lit -sv test/lit  with GOC_BUILD / LLVM_BIN set.
import os
import lit.formats

config.name = "goc"
config.test_format = lit.formats.ShTest(execute_external=True)
config.suffixes = [".ll"]
config.test_source_root = os.path.dirname(__file__)
root = os.path.abspath(os.path.join(config.test_source_root, "..", ".."))
build = os.environ.get("GOC_BUILD", os.path.join(root, "build", "llvm"))
llvm_bin = os.environ.get("LLVM_BIN", "/usr/lib/llvm-19/bin")
config.test_exec_root = os.environ.get("GOC_LIT_OUT", os.path.join(root, "build", "lit"))

for var in ("HOME", "TMPDIR"):
    if var in os.environ:
        config.environment[var] = os.environ[var]

def tool(path):
    if not os.access(path, os.X_OK):
        lit_config.fatal("missing %s (build it with scripts/build-passes.sh)" % path)
    return path

config.substitutions.append(("%goc-color-escape", tool(os.path.join(build, "goc-color-escape"))))
config.substitutions.append(("%goc-llc", tool(os.path.join(build, "goc-llc"))))
config.substitutions.append(("%opt-goc", "%s -load-pass-plugin=%s" % (
    tool(os.path.join(llvm_bin, "opt")), tool(os.path.join(build, "libGocPasses.so")))))
# FileCheck and not come from LLVM_BIN, ahead of anything else on PATH.
for t in ("FileCheck", "not"):
    tool(os.path.join(llvm_bin, t))
config.environment["PATH"] = os.pathsep.join([llvm_bin, os.environ.get("PATH", "")])

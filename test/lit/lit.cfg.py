# lit configuration: the patched clang's Sema for the pointer colors
# (Sema/), goc's LLVM passes (ColorEscape/, StackMap/) and goc-llc
# (CodeGen/). Run with test/lit/run.sh (it finds lit and FileCheck), or
#   lit -sv test/lit  with GOC_BUILD / LLVM_BIN set.
import os
import lit.formats

config.name = "goc"
config.test_format = lit.formats.ShTest()  # internal shell; lit 23 rejects execute_external=True
config.suffixes = [".ll", ".c"]
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

# The patched clang next to goc: bin/clang in a release, the clang build
# tree in a checkout (as internal/toolchain finds it).
clang = os.path.join(root, "bin", "clang")
if not os.access(clang, os.X_OK):
    clang = os.path.join(root, "third_party", "llvm-19.1.7-clang-build", "bin", "clang")
clang = tool(clang)
libdir = os.path.join(os.path.dirname(os.path.dirname(os.path.realpath(clang))), "lib")
config.environment["LD_LIBRARY_PATH"] = os.pathsep.join(
    [libdir, os.environ.get("LD_LIBRARY_PATH", "")])
goc_clang = "%s -I %s" % (clang, os.path.join(root, "include"))
# %goc-clang-ll: C to the unoptimized IR the passes see (as goc build).
config.substitutions.append(("%goc-clang-ll",
    goc_clang + " -emit-llvm -S -O0 -Xclang -disable-O0-optnone"))
config.substitutions.append(("%goc-clang", goc_clang))
config.substitutions.append(("%goc-llc", tool(os.path.join(build, "goc-llc"))))
# opt of the clang tree, which libGocPasses.so is built against (as goc runs it).
opt_goc = "%s -load-pass-plugin=%s" % (
    tool(os.path.join(os.path.dirname(os.path.realpath(clang)), "opt")), tool(os.path.join(build, "libGocPasses.so")))
# Longest first: lit substitutes in order.
config.substitutions.append(("%goc-color-escape", opt_goc + " -passes=goc-color-escape -S"))
config.substitutions.append(("%opt-goc", opt_goc))
# FileCheck and not come from LLVM_BIN, ahead of anything else on PATH.
for t in ("FileCheck", "not"):
    tool(os.path.join(llvm_bin, t))
config.environment["PATH"] = os.pathsep.join([llvm_bin, os.environ.get("PATH", "")])

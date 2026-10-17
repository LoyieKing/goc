package convert

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

// goEnv returns one `go env` value of the go command in PATH.
func goEnv(name string) (string, error) {
	cmd := exec.Command("go", "env", name)
	cmd.Env = append(os.Environ(), "GOTOOLCHAIN=local")
	b, err := cmd.Output()
	if err != nil {
		return "", fmt.Errorf("go env %s: %v (is go in PATH?)", name, err)
	}
	return strings.TrimSpace(string(b)), nil
}

// objectHeader returns the header line the linker of the go command in
// PATH expects (objabi.HeaderString: release, GOARCH variant, enabled
// experiments). It is read from an object `go tool asm` writes for an
// empty file under the same environment, and cached per environment.
func objectHeader(goarch, ver string) (string, error) {
	cmd := exec.Command("go", "env", "-json", "GOROOT", "GOVERSION", "GOOS", "GOEXPERIMENT", "GOAMD64", "GOARM64")
	cmd.Env = append(os.Environ(), "GOTOOLCHAIN=local", "GOARCH="+goarch)
	b, err := cmd.Output()
	if err != nil {
		return "", fmt.Errorf("go env: %v (is go in PATH?)", err)
	}
	var env map[string]string
	if err := json.Unmarshal(b, &env); err != nil {
		return "", err
	}
	if env["GOVERSION"] != ver {
		return "", fmt.Errorf("go in PATH is %s, objects are being written for %s: pass -header", env["GOVERSION"], ver)
	}
	sum := sha256.Sum256(append(b, goarch...))
	key := hex.EncodeToString(sum[:12])
	var cache string
	if dir, err := os.UserCacheDir(); err == nil {
		cache = filepath.Join(dir, "goc", "objheader", key)
		if h, err := os.ReadFile(cache); err == nil && strings.HasPrefix(string(h), "go object ") {
			return string(h), nil
		}
	}
	tmp, err := os.MkdirTemp("", "goc-hdr")
	if err != nil {
		return "", err
	}
	defer os.RemoveAll(tmp)
	src := filepath.Join(tmp, "x.s")
	obj := filepath.Join(tmp, "x.o")
	if err := os.WriteFile(src, nil, 0o644); err != nil {
		return "", err
	}
	asm := exec.Command("go", "tool", "asm", "-p", "main", "-o", obj, src)
	asm.Env = append(os.Environ(), "GOTOOLCHAIN=local", "GOARCH="+goarch)
	if out, err := asm.CombinedOutput(); err != nil {
		return "", fmt.Errorf("go tool asm: %v\n%s", err, out)
	}
	o, err := os.ReadFile(obj)
	if err != nil {
		return "", err
	}
	line, _, ok := strings.Cut(string(o), "\n")
	if !ok || !strings.HasPrefix(line, "go object ") {
		return "", fmt.Errorf("go tool asm wrote no object header")
	}
	h := line + "\n"
	if cache != "" {
		if os.MkdirAll(filepath.Dir(cache), 0o755) == nil {
			t := cache + fmt.Sprintf(".%d", os.Getpid())
			if os.WriteFile(t, []byte(h), 0o644) == nil {
				os.Rename(t, cache)
			}
		}
	}
	return h, nil
}

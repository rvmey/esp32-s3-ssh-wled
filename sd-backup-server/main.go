package main

import (
	"flag"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

// uploadDir is the base directory under which uploaded files are stored.
var uploadDir string

func main() {
	addr := flag.String("addr", ":8080", "host:port to listen on")
	flag.StringVar(&uploadDir, "dir", "uploads", "base directory for stored uploads")
	maxMem := flag.Int64("maxmem", 32<<20, "max bytes kept in memory while parsing multipart form")
	flag.Parse()

	if err := os.MkdirAll(uploadDir, 0o755); err != nil {
		log.Fatalf("cannot create upload dir %q: %v", uploadDir, err)
	}

	mux := http.NewServeMux()
	mux.HandleFunc("/upload", uploadHandler(*maxMem))
	mux.HandleFunc("/probe", probeHandler)

	log.Printf("listening on %s, storing uploads under %q", *addr, uploadDir)
	if err := http.ListenAndServe(*addr, mux); err != nil {
		log.Fatal(err)
	}
}

func uploadHandler(maxMem int64) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}

		if err := r.ParseMultipartForm(maxMem); err != nil {
			http.Error(w, "invalid multipart form: "+err.Error(), http.StatusBadRequest)
			return
		}

		file, _, err := r.FormFile("file")
		if err != nil {
			http.Error(w, "missing 'file' field: "+err.Error(), http.StatusBadRequest)
			return
		}
		defer file.Close()

		// request.form['path'] = destination path (incl. filename) relative to uploadDir.
		relPath := r.FormValue("path")
		if relPath == "" {
			http.Error(w, "missing 'path' field", http.StatusBadRequest)
			return
		}
		dest, err := safeJoin(uploadDir, relPath)
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}

		// Chunked upload: write this chunk's bytes at 'offset' within the file.
		// Chunks arrive in order from offset 0, so offset 0 truncates/creates
		// and later chunks are written at their offset.
		offset, err := parseInt(r.FormValue("offset"), 0)
		if err != nil {
			http.Error(w, "invalid 'offset' field: "+err.Error(), http.StatusBadRequest)
			return
		}

		if err := os.MkdirAll(filepath.Dir(dest), 0o755); err != nil {
			http.Error(w, "cannot create directory: "+err.Error(), http.StatusInternalServerError)
			return
		}

		// offset 0 creates/truncates; later chunks open for writing at offset.
		flags := os.O_WRONLY | os.O_CREATE
		if offset == 0 {
			flags |= os.O_TRUNC
		}
		out, err := os.OpenFile(dest, flags, 0o644)
		if err != nil {
			http.Error(w, "cannot open file: "+err.Error(), http.StatusInternalServerError)
			return
		}
		defer out.Close()

		if _, err := out.Seek(offset, io.SeekStart); err != nil {
			http.Error(w, "seek failed: "+err.Error(), http.StatusInternalServerError)
			return
		}

		n, err := io.Copy(out, file)
		if err != nil {
			http.Error(w, "write failed: "+err.Error(), http.StatusInternalServerError)
			return
		}

		// Optional 'total' field lets the server report completion.
		end := offset + n
		complete := false
		if totalStr := r.FormValue("total"); totalStr != "" {
			total, err := parseInt(totalStr, 0)
			if err != nil {
				http.Error(w, "invalid 'total' field: "+err.Error(), http.StatusBadRequest)
				return
			}
			complete = end == total
		}

		log.Printf("wrote %s [%d-%d] complete=%v", dest, offset, end, complete)
		fmt.Fprint(w, "ok\n")
	}
}

// probeHandler answers GET /probe?path=<rel>&total=<size>.
// Returns 200 if the file exists at exactly 'total' bytes, 404 otherwise.
func probeHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	relPath := r.URL.Query().Get("path")
	if relPath == "" {
		http.Error(w, "missing 'path' param", http.StatusBadRequest)
		return
	}
	total, err := parseInt(r.URL.Query().Get("total"), 0)
	if err != nil || total <= 0 {
		http.Error(w, "invalid 'total' param", http.StatusBadRequest)
		return
	}

	dest, err := safeJoin(uploadDir, relPath)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}

	info, err := os.Stat(dest)
	if err == nil && info.Size() == total {
		log.Printf("probe %s: exists (%d bytes)", dest, total)
		fmt.Fprint(w, "exists\n")
		return
	}

	log.Printf("probe %s: not found or size mismatch", dest)
	http.NotFound(w, r)
}

// parseInt parses a base-10 int64, returning def for an empty string.
func parseInt(s string, def int64) (int64, error) {
	if s == "" {
		return def, nil
	}
	return strconv.ParseInt(s, 10, 64)
}

// safeJoin combines base with a client-supplied relative path, ensuring the
// result stays inside base (guards against path traversal like "../").
func safeJoin(base, relPath string) (string, error) {
	clean := filepath.Clean(filepath.FromSlash(relPath))
	dest := filepath.Join(base, clean)

	absBase, err := filepath.Abs(base)
	if err != nil {
		return "", err
	}
	absDest, err := filepath.Abs(dest)
	if err != nil {
		return "", err
	}
	if absDest != absBase && !strings.HasPrefix(absDest, absBase+string(filepath.Separator)) {
		return "", fmt.Errorf("path escapes upload directory")
	}
	return dest, nil
}

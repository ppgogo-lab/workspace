#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "Usage: $0 <input.pdf> [output.txt]" >&2
  exit 1
fi

input_pdf="$1"
output_txt="${2:-}"

if [[ ! -f "$input_pdf" ]]; then
  echo "Input PDF not found: $input_pdf" >&2
  exit 1
fi

run_python_fallback() {
  python3 - "$input_pdf" "${output_txt:-}" <<'PY'
import sys
from pathlib import Path

pdf_path = Path(sys.argv[1])
output_path = Path(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2] else None

extractors = []

try:
    from pypdf import PdfReader
    def extract_pypdf(path: Path) -> str:
        reader = PdfReader(str(path))
        return "\n\n".join(page.extract_text() or "" for page in reader.pages)
    extractors.append(("pypdf", extract_pypdf))
except Exception:
    pass

try:
    import PyPDF2
    def extract_pypdf2(path: Path) -> str:
        with path.open("rb") as fh:
            reader = PyPDF2.PdfReader(fh)
            return "\n\n".join(page.extract_text() or "" for page in reader.pages)
    extractors.append(("PyPDF2", extract_pypdf2))
except Exception:
    pass

try:
    import fitz
    def extract_pymupdf(path: Path) -> str:
        doc = fitz.open(str(path))
        return "\n\n".join(page.get_text() or "" for page in doc)
    extractors.append(("pymupdf", extract_pymupdf))
except Exception:
    pass

if not extractors:
    print("No PDF text extraction backend found.", file=sys.stderr)
    print("Install one of:", file=sys.stderr)
    print("  sudo apt install -y poppler-utils", file=sys.stderr)
    print("  python3 -m pip install --user pypdf", file=sys.stderr)
    print("  python3 -m pip install --user pymupdf", file=sys.stderr)
    sys.exit(2)

backend_name, extractor = extractors[0]
text = extractor(pdf_path)

if output_path:
    output_path.write_text(text, encoding="utf-8")
    print(f"Extracted with {backend_name}: {output_path}")
else:
    sys.stdout.write(text)
PY
}

if command -v pdftotext >/dev/null 2>&1; then
  if [[ -n "$output_txt" ]]; then
    pdftotext "$input_pdf" "$output_txt"
    echo "Extracted with pdftotext: $output_txt"
  else
    pdftotext "$input_pdf" -
  fi
  exit 0
fi

if command -v python3 >/dev/null 2>&1; then
  run_python_fallback
  exit 0
fi

echo "Neither pdftotext nor python3 is available in WSL." >&2
echo "Install one of:" >&2
echo "  sudo apt install -y poppler-utils python3" >&2
exit 2

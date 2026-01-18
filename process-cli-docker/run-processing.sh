#!/usr/bin/env bash
set -euo pipefail

INPUT_DIR="${INPUT_DIR:-/data/input}"
OUTPUT_DIR="${OUTPUT_DIR:-/data/output}"

if [[ ! -d "$INPUT_DIR" ]]; then
  echo "Input directory not found: $INPUT_DIR" >&2
  exit 1
fi

mkdir -p "$OUTPUT_DIR"

processed=0
errors=0

while IFS= read -r -d '' file; do
  rel_path="${file#$INPUT_DIR/}"
  rel_dir="$(dirname "$rel_path")"

  if [[ "$rel_dir" == "." ]]; then
    target_dir="$OUTPUT_DIR"
  else
    target_dir="$OUTPUT_DIR/$rel_dir"
  fi

  mkdir -p "$target_dir"
  echo "Processing: $file -> $target_dir"

  if node /app/cli.js "$file" -o "$target_dir"; then
    processed=$((processed + 1))
  else
    errors=$((errors + 1))
  fi
done < <(
  find "$INPUT_DIR" -type f \( \
    -iname "*.jpg" -o -iname "*.jpeg" -o -iname "*.png" -o \
    -iname "*.gif" -o -iname "*.bmp" -o -iname "*.webp" -o \
    -iname "*.heic" -o -iname "*.heif" \
  \) -print0
)

if [[ "$processed" -eq 0 ]]; then
  echo "No images found in input directory."
  exit 0
fi

echo "Processed images: $processed"
if [[ "$errors" -gt 0 ]]; then
  echo "Errors: $errors" >&2
  exit 1
fi

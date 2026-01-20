.PHONY: format format-check format-diff test help

# Find all C and H files in main/ directory only
# Exclude components/ (vendor library code), build/, managed_components/, etc.
C_FILES := $(shell find main -type f \( -name "*.c" -o -name "*.h" \) 2>/dev/null)

# Find all JS files in main/webapp/ and process-cli/ directories
JS_FILES := $(shell find main/webapp process-cli -type f -name "*.js" 2>/dev/null | grep -v node_modules)

# Find all Python files in the project root and docs/
PY_FILES := $(shell find . -maxdepth 1 -type f -name "*.py" 2>/dev/null) \
	    $(shell find scripts -type f -name "*.py" 3>/dev/null)

help:
	@echo "Available targets:"
	@echo "  format        - Format all C/H/JS/Python files with clang-format, prettier, black, and isort"
	@echo "  format-check  - Check if files need formatting (non-zero exit if changes needed)"
	@echo "  format-diff   - Show what would change without modifying files"
	@echo "  test          - Build and run unit tests (requires ESP-IDF environment)"

format:
	@echo "Formatting C/H files..."
	@clang-format -i $(C_FILES)
	@echo "Done! Formatted $(words $(C_FILES)) C/H files."
	@echo "Formatting JS files..."
	@npx prettier --write $(JS_FILES)
	@echo "Done! Formatted $(words $(JS_FILES)) JS files."
	@if [ -n "$(PY_FILES)" ]; then \
		echo "Formatting Python files with isort..."; \
		python3 -m isort $(PY_FILES); \
		echo "Formatting Python files with black..."; \
		python3 -m black $(PY_FILES); \
		echo "Done! Formatted $(words $(PY_FILES)) Python files."; \
	fi

format-check:
	@echo "Checking C/H files formatting..."
	@clang-format --dry-run --Werror $(C_FILES)
	@echo "Checking JS files formatting..."
	@npx prettier --check $(JS_FILES)
	@if [ -n "$(PY_FILES)" ]; then \
		echo "Checking Python files formatting..."; \
		python3 -m isort --check-only $(PY_FILES); \
		python3 -m black --check $(PY_FILES); \
	fi
	@echo "All files are properly formatted!"

format-diff:
	@echo "Showing formatting differences for C/H files..."
	@for file in $(C_FILES); do \
		echo "=== $$file ==="; \
		clang-format "$$file" | diff -u "$$file" - || true; \
	done
	@echo "Showing formatting differences for JS files..."
	@for file in $(JS_FILES); do \
		echo "=== $$file ==="; \
		npx prettier "$$file" | diff -u "$$file" - || true; \
	done
	@if [ -n "$(PY_FILES)" ]; then \
		echo "Showing formatting differences for Python files..."; \
		for file in $(PY_FILES); do \
			echo "=== $$file ==="; \
			python3 -m black --diff "$$file" || true; \
		done; \
	fi

test:
	@echo "Building and running host-based unit tests..."
	@mkdir -p host_tests/build
	@cd host_tests/build && cmake .. && make
	@echo ""
	@echo "Running tests..."
	@./host_tests/build/utils_test
	@echo ""
	@echo "✓ All tests passed!"

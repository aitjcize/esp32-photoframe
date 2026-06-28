.PHONY: format format-check format-diff test help install-hooks

# Use clang-format-18 for consistency with CI
# On macOS: brew install llvm@18 && brew link llvm@18
# On Ubuntu: sudo apt-get install clang-format-18
CLANG_FORMAT := $(shell which clang-format-18 2>/dev/null || which /opt/homebrew/opt/llvm@18/bin/clang-format 2>/dev/null || echo clang-format)

# Find all C and H files in main/ and components/ directories
# Exclude build/, managed_components/, etc.
C_FILES := $(shell find main -type f \( -name "*.c" -o -name "*.cpp" -o -name "*.h" -o -name "*.hpp" \) 2>/dev/null) \
	   $(shell find components -type f \( -name "*.c" -o -name "*.cpp" -o -name "*.h" -o -name "*.hpp" \) 2>/dev/null) \
	   $(shell find host_tests -type f \( -name "*.c" -o -name "*.cpp" -o -name "*.h" -o -name "*.hpp" \) 2>/dev/null)

# Find all Python files in the project root and docs/
PY_FILES := $(shell find main -type f -name "*.py" 2>/dev/null) \
	    $(shell find scripts -type f -name "*.py" 3>/dev/null)

help:
	@echo "Available targets:"
	@echo "  format        - Format all C/H/Python and JS/Vue files"
	@echo "  format-check  - Check if files need formatting (non-zero exit if changes needed)"
	@echo "  format-diff   - Show what would change without modifying files"
	@echo "  test          - Build and run unit tests (requires ESP-IDF environment)"
	@echo "  install-hooks - Enable the git pre-commit formatting hook (.githooks)"

install-hooks:
	@git config core.hooksPath .githooks
	@echo "Git hooks enabled (core.hooksPath = .githooks)."
	@echo "Commits now run 'make format-check' first."

format:
	@echo "Formatting C/H files..."
	@$(CLANG_FORMAT) -i $(C_FILES)
	@echo "Done! Formatted $(words $(C_FILES)) C/H files."
	@if [ -n "$(PY_FILES)" ]; then \
		echo "Formatting Python files with isort..."; \
		python3 -m isort $(PY_FILES); \
		echo "Formatting Python files with black..."; \
		python3 -m black $(PY_FILES); \
		echo "Done! Formatted $(words $(PY_FILES)) Python files."; \
	fi
	@cd webapp && npm run format
	@echo "Done! Formatted webapp files."
	@echo "Formatting process-cli JS files..."
	@cd process-cli && npm run format
	@echo "Done! Formatted process-cli files."

format-check:
	@echo "Checking C/H files formatting..."
	@$(CLANG_FORMAT) --dry-run --Werror $(C_FILES)
	@if [ -n "$(PY_FILES)" ]; then \
		echo "Checking Python files formatting..."; \
		python3 -m isort --check-only $(PY_FILES); \
		python3 -m black --check $(PY_FILES); \
	fi
	@echo "Checking webapp Vue files formatting..."
	@cd webapp && [ -d node_modules ] || npm ci
	@cd webapp && npm run format:check
	@echo "Checking process-cli JS files formatting..."
	@cd process-cli && [ -d node_modules ] || npm ci
	@cd process-cli && npm run format:check
	@echo "All files are properly formatted!"

format-diff:
	@echo "Showing formatting differences for C/H files..."
	@for file in $(C_FILES); do \
		echo "=== $$file ==="; \
		$(CLANG_FORMAT) "$$file" | diff -u "$$file" - || true; \
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
	@echo "Running C unit tests..."
	@./host_tests/build/utils_test
	@echo ""
	@echo "Running image orientation tests..."
	@cd process-cli && npm install --silent && npm run test:orientation
	@echo ""
	@echo "✓ All tests passed!"

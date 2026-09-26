# C style

Use the repository's LLVM-based `.clang-format` configuration for C source and
headers. Formatting should make control flow easier to read while preserving
behavior, existing identifiers, comments, and include order.

## Conventions

- Indent with four spaces. Put opening braces on the same line as the function,
  conditional, or loop; put `else` after the preceding closing brace.
- Write one statement per line. Use braces for `if`, `else`, `for`, `while`, and
  `do` bodies, including a body containing only one statement.
- Aim for 100 columns. Unbreakable strings and macros can exceed the limit;
  do not change their contents or meaning merely to wrap a line.
- Separate functions and logical groups of statements with a blank line. Keep
  related operations together without adding a blank line after every statement.
- Preserve existing naming: `vs_function`, `VSType`, and `VS_CONSTANT` in the
  scanner, and the established names in the helper and tests. Formatting is not
  a reason to rename symbols or add prefixes to local helpers.
- Prefer explicit branches to complicated nested ternary expressions. A short,
  simple ternary is fine. This is a readability judgment for code changes;
  the formatter does not rewrite expressions or choose logical groups.

For example:

```c
void *vs_calloc(size_t n, size_t size) {
    if (size && n > SIZE_MAX / size) {
        vs_die("allocation multiplication overflow");
    }

    size_t bytes = n * size;
    void *p = vs_alloc(bytes);
    memset(p, 0, bytes);
    return p;
}
```

## Reproducible formatting

Use **clang-format 22.1.0**, pinned in
[`tools/requirements-format.txt`](../tools/requirements-format.txt). The format
commands validate the executable's version so different developer or CI
installations do not produce competing styles. To install the pinned version
in an ignored local environment, run from the repository root:

```sh
python3 -m venv tmp/clang-format
tmp/clang-format/bin/python -m pip install -r tools/requirements-format.txt
make format
make format-check
```

Make automatically uses `tmp/clang-format/bin/clang-format` when present,
otherwise it looks for `clang-format` on `PATH`. Set
`CLANG_FORMAT=/path/to/clang-format` to use another installation of the same
version. The formatter is a development tool, not a runtime or ordinary build
dependency. CI installs the same requirements file and runs `make format-check`
in a separate job.

`make format` updates `.c` and `.h` files under `src/`, `cli/`, and `tests/`.
It excludes the entire `tests/fixtures/` subtree: captured test inputs and their
checksums must remain unchanged. `make format-check` checks the same files
without writing them and fails if any need formatting.

Formatting is idempotent: after `make format` succeeds, repeating it with the
same source and pinned tool leaves file contents and modification times
unchanged, and `make format-check` passes. The runner compares the formatted
output before writing, stages changed files under ignored `tmp/`, and replaces
each changed file atomically. Check mode never writes files.

Review the diff and run the relevant existing tests after formatting, especially
where braces or preprocessor directives are involved. Formatting alone must not
change behavior.

The [clang-format style options reference](https://clang.llvm.org/docs/ClangFormatStyleOptions.html)
describes the settings used by `.clang-format`.

# Style and conventions
- Language style: modern C++ with `#pragma once`, STL containers, `std::span`, strong use of `using` aliases for IDs, and inline method definitions in the header.
- Naming: class names use PascalCase, methods and fields use snake_case, function parameters are prefixed with `a_`, member fields use `m_`, and constants use `k_`.
- Comments: concise Japanese comments with numbered steps are used to describe intent inside methods.
- Error handling: invariants are enforced with `std::runtime_error` / `std::overflow_error`; sample app uses `assert` for demonstration failures.
- Formatting: UTF-8 files; `.editorconfig` only specifies charset for `*.cpp`, `*.h`, `*.hlsl`, `*.hlsli`.
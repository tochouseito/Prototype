# Prototype overview
- Purpose: small C++20 Visual Studio console prototype for experimenting with a `BatchedRegistry` that stores integer items, assigns stable item/batch IDs, and supports erasing by item or whole batch.
- Tech stack: C++20, MSVC/Visual Studio toolchain, Windows console app.
- Structure: repo root contains `Prototype.slnx`, `.editorconfig`, and the `Prototype/` project directory. Core logic currently lives in `Prototype/BatchedNumberRegistry.h`; sample usage/entrypoint is `Prototype/Prototype.cpp`.
- Notes: this is a lightweight prototype rather than a full library package; behavior is demonstrated through the console executable.
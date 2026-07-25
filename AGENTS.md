# AGENTS.md

## Project

This repository contains MSPM0G3507 line-following car firmware.

The main development environment is VS Code with Keil Studio / CMSIS Solution
and Arm Compiler 6. Windows Keil uVision is also valid.

The related MSPM0L1306 sensor auxiliary-board source may be located at:

```
/Users/metro/Downloads/MSPM0L1306辅助板开源/MSPM0L1306辅助板源码/NO_MCU_BOARD
```

The car and auxiliary board are separate firmware projects.

## Protocol Changes

The car and auxiliary board communicate through CLK/DAT.

When changing protocol-related behavior, inspect both projects and update both
sides when required.

Protocol-related behavior includes:

- frame layout
- bit order
- CRC
- timing
- polarity
- channel order
- sequence or timeout behavior

Do not fix one side while knowingly leaving the other side incompatible.

## Editing

Read the relevant current code before editing.

Preserve existing user changes and avoid unrelated rewrites.

Follow the repository's existing C99 style and module boundaries.

Do not manually edit build artifacts or generated output files.

Do not modify Keil project configuration, startup files, linker files,
SysConfig-generated files, or device configuration unless the task actually
requires a project, pin, peripheral, or memory configuration change.

Keep interrupt handlers short.

Avoid blocking operations in the 5 ms sensor path and 10 ms control path.

Shared ISR and main-loop state must be handled safely.

## Working Style

Complete the requested task as fully as practical.

Changes may span multiple modules or both firmware projects when the problem
requires it. Do not artificially split a coherent fix into tiny patches.

Prefer fixing the root cause instead of compensating for invalid sensor data,
timing bugs, or protocol errors with PID tuning.

Do not erase, flash, reset, or run connected hardware unless the user explicitly
asks for hardware operations.

## Build and Verification

Prefer Keil Studio CMSIS Solution builds.

Primary project files include:

- `empty_LP_MSPM0G3507_nortos_keil.csolution.yml`
- `keil/empty_LP_MSPM0G3507_nortos_keil.cproject.yml`

The uVision project is also valid:

- `keil/empty_LP_MSPM0G3507_nortos_keil.uvprojx`

Use `cbuild` when it is available and uses the repository's CMSIS Solution.

Do not substitute CCS or an unrelated compiler just to claim build success.

When a local Keil build is unavailable, perform useful static verification and
clearly state that firmware compilation was not verified.

Useful checks include:

- focused diff review
- symbol and reference search
- protocol consistency checks
- `git diff --check`

Compilation success and on-board runtime validation are separate results.



Fix invalid data and timing problems before tuning control parameters.

## Core Principles

### Think Before Coding

Do not silently guess. Read the relevant code and state important assumptions.
When multiple interpretations exist, surface them before implementing.

### Simplicity First

Write the minimum code needed to solve the requested problem.

Do not add speculative features, single-use abstractions, unrequested
configurability, or defensive handling for impossible scenarios.

If the implementation is substantially larger than necessary, simplify it.

### Surgical Changes

Touch only what the task requires.

Do not refactor, reformat, rename, or clean up unrelated existing code.
Match the repository's current style and remove only leftovers created by your
own changes.

Every changed line should trace directly to the user's request.

### Goal-Driven Execution

Translate the task into observable success criteria before editing.

Implement the smallest coherent fix, verify the affected path, and inspect the
final diff for unnecessary changes.
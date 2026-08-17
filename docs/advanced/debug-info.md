# Debug Info (DWARF)

Pass `-g` (or `--debug`) to embed DWARF debug information in the output.

```sh
utopia build path/to/project -g
```

## What is emitted

- **Compile units** with language `DW_LANG_C`, DWARF version 4, and the current file path.
- **Subprograms** (`DW_TAG_subprogram`) with full subroutine types, including the `this` parameter of methods.
- **Lexical blocks** (`DW_TAG_lexical_block`) matching source blocks.
- **Source locations** (`DW_TAG_lexical_block`/`DW_TAG_call_site` via `DILocation`) attached to most instructions.
- **Variables**:
  - locals (`DW_TAG_variable` with `DW_OP_fbreg`/`DW_OP_deref` expressions via `llvm.dbg.declare`),
  - parameters (`DW_TAG_formal_parameter`),
  - globals (`DW_TAG_variable` + `llvm.dbg.global`).
- **Types**:
  - basic types with byte sizes,
  - pointers, references, and `const` (`DW_TAG_const_type`),
  - arrays with subrange sizes,
  - structs/classes (`DW_TAG_structure_type`) and unions (`DW_TAG_union_type`) with members at real bit offsets (forward declarations resolved after layout),
  - typedefs, enums, and function types.

## Usage

```sh
gdb ./build/bin/my_game
(gdb) break main
(gdb) run
(gdb) print myVariable
(gdb) bt
```

## Notes

- Debug metadata is emitted alongside TBAA metadata and lifetime intrinsics.
- The LSP and other tools consume the same source positions used for debug info.

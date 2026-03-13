# miniC Compiler

This repo contains the full pipeline for a compiler I built for my COSC 057 class. It takes a simplified C-like language ("miniC") and translates it all the way down to x86-32 assembly.

## Structure

It's broken down into four main parts:

1. **Frontend (`Part1 Front End/` / `Frontend/`)** 
   - Uses Flex and Bison (Lex/Yacc) to scan and parse raw `.c` code into an Abstract Syntax Tree (AST). 
   - Includes semantic analysis for type checking.

2. **IR Builder (`LLVM IR Builder/`)**
   - Walks the AST and translates it into unoptimized LLVM Intermediate Representation (`.ll`).
   - Uses the LLVM-C API (`LLVMBuildLoad`, `LLVMBuildBr`, etc) to generate the basic blocks and instructions.

3. **Optimizer (`Optimizations/`)**
   - The middle-end. Takes the `.ll` files and runs local/global optimization passes on the basic blocks.
   - Handles Common Subexpression Elimination (CSE), Dead Code Elimination (DCE), and Constant Folding.

4. **Backend (`backend/`)**
   - Takes the optimized LLVM IR and generates actual x86 32-bit AT&T assembly (`.s`).
   - Implements a local linear-scan register allocation algorithm to pick physical registers (`%ebx`, `%ecx`, `%edx`) and spill variables to the stack when needed.

## Building

Each folder has its own Makefile. You basically run `make` in each stage. 

The LLVM-C API requires an environment with `llvm-config-17` and the dev headers installed.

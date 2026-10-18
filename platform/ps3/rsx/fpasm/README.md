# Fragment program assembler (from PSL1GHT's cgcomp)

The NV40 fragment program parser and compiler of PSL1GHT's `cgcomp` tool
(https://github.com/ps3dev/PSL1GHT, `tools/cgcomp`, commit f649a08), used by the PS3 renderer to assemble
the fragment programs it generates at run time (`../fp_assembler.cpp`). MIT licence: see
`LICENSE.PSL1GHT`; `nv30-40_3d.xml.h` and `nvfx_shader.h` come from the nouveau/Mesa projects under the
same terms (notices in the files).

Changes for the port, marked "(PS3 port)":
- errors are reported through `GetError()` instead of `exit()`, `throw` and messages on stderr (the game
  is built without exceptions, and must not quit over a bad program);
- an unknown instruction or a texture lookup without a `texture[N]` unit is an error (it was dropped
  silently);
- the parser frees its parameter values, and the compiler its instructions;
- all memory comes from malloc (containers with `gfx::MallocAllocator`, see `../rsx_alloc.h`): the game
  replaces the global operator new with its own heaps. The static tables hold `const char*` instead of
  `std::string` (their constructors ran before `main` and crashed the game at start-up), and the parser
  splits lines itself instead of using a `std::stringstream`.

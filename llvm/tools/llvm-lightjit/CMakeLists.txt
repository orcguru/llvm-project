set(LLVM_LINK_COMPONENTS
  LightJIT
  )

add_llvm_tool(llvm-lightjit
  llvm-lightjit.c

  EXPORT_SYMBOLS
  )

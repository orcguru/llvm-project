add_llvm_component_library(LLVMLightJIT
  light_jitlink.cpp
  qemu_lightjit.cpp

  LINK_COMPONENTS
  BinaryFormat
  Object
  Support
  )

target_link_libraries(LLVMLightJIT
  PRIVATE
  LLVMObject
  LLVMSupport
)

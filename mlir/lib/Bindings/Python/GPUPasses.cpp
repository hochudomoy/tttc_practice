//===- GPUPasses.cpp - Pybind module for the GPU passes ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===---------------------------------------------------------------------===//

#include "mlir-c/Dialect/GPU.h"

#include "mlir/Bindings/Python/Nanobind.h"

// -----------------------------------------------------------------------------
// Module initialization.
// -----------------------------------------------------------------------------

NB_MODULE(_mlirGPUPasses, m) {
  m.doc() = "MLIR GPU Dialect Passes";

  // Register all GPU passes on load.
  mlirRegisterGPUPasses();
}

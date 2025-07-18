/* Copyright 2024 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
// TODO(ROCm): Enable and include ROCm Triton passes when ROCm Triton is
// included in build.
#include <string>
#include <utility>

#include "third_party/amd/include/TritonAMDGPUToLLVM/Passes.h"
#include "third_party/amd/include/TritonAMDGPUTransforms/Passes.h"
#include "mlir/Conversion/ArithToLLVM/ArithToLLVM.h"
#include "mlir/Conversion/ControlFlowToLLVM/ControlFlowToLLVM.h"
#include "mlir/Conversion/IndexToLLVM/IndexToLLVM.h"
#include "mlir/Conversion/SCFToControlFlow/SCFToControlFlow.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Transforms/Passes.h"
#include "xla/backends/gpu/codegen/triton/transforms/passes.h"
#include "xla/service/gpu/llvm_gpu_backend/amdgpu_backend.h"
#include "xla/service/gpu/matmul_utils.h"
#include "xla/service/hlo_module_config.h"
#include "xla/stream_executor/device_description.h"
#include "xla/tsl/platform/rocm_rocdl_path.h"
#include "triton/Conversion/TritonGPUToLLVM/Passes.h"
#include "triton/Conversion/TritonToTritonGPU/Passes.h"
#include "triton/Dialect/Triton/Transforms/Passes.h"
#include "triton/Dialect/TritonGPU/Transforms/Passes.h"
#include "triton/Dialect/TritonNvidiaGPU/Transforms/Passes.h"

namespace xla {
namespace gpu {

namespace ma = ::mlir::arith;
namespace mm = ::mlir::math;
namespace ml = ::mlir::LLVM;
namespace mt = ::mlir::triton;
namespace mt_xla = ::mlir::triton::xla;

using ::llvm::SmallVector;
using mlir::ArrayRef;
using ::mlir::ShapedType;
using ::mlir::Type;
using ::mlir::Value;
using mlir::ValueRange;

absl::Status CreateTritonPipeline(
    mlir::OpPassManager* pm, std::string arch_name, int num_warps, int num_ctas,
    int num_stages, mt::nvidia_gpu::ClusterInfo& out_cluster_info) {
  const int threadsPerWarp = (arch_name[3] == '9') ? 64 : 32;
  auto cc = se::RocmComputeCapability(std::move(arch_name));

  // Based on make_ttir() in
  // @triton//:third_party/amd/backend/compiler.py
  pm->addPass(mlir::createInlinerPass());
  pm->addPass(mt::createTritonRewriteTensorPointer());
  pm->addPass(mt::createTritonRewriteTensorDescriptorToPointer());
  pm->addPass(mlir::createCanonicalizerPass());
  pm->addPass(mt::createTritonCombineOps());
  pm->addPass(mt::createTritonReorderBroadcast());
  pm->addPass(mlir::createCSEPass());
  pm->addPass(mlir::createLoopInvariantCodeMotionPass());
  pm->addPass(mlir::createSymbolDCEPass());
  pm->addPass(mt::createTritonLoopUnroll());

  // Based on make_ttgir() in
  // @triton//:third_party/amd/backend/compiler.py
  pm->addPass(mt::createConvertTritonToTritonGPU(
      {absl::StrCat("hip:", cc.gfx_version()), num_warps, threadsPerWarp,
       num_ctas}));
  pm->addPass(mt::gpu::createTritonGPUCoalesce());
  pm->addPass(mt::gpu::createTritonGPURemoveLayoutConversions());
  pm->addPass(mt::gpu::createTritonGPUOptimizeThreadLocality());
  // TODO ROCm Pass cc.gfx_version() after fixing issue with fmfa
  pm->addPass(mlir::createTritonAMDGPUAccelerateMatmul({arch_name}));
  pm->addPass(mt::gpu::createTritonGPURemoveLayoutConversions());
  // TODO ROCm Check if we want to compare MI100 and greater
  pm->addPass(mlir::createTritonAMDGPUOptimizeEpilogue());
  pm->addPass(mt::gpu::createTritonGPUOptimizeDotOperands({true}));
  pm->addNestedPass<mlir::triton::FuncOp>(
      mlir::createTritonAMDGPUHoistLayoutConversions());

  pm->addPass(mt::gpu::createTritonGPUFuseNestedLoops());
  pm->addPass(mlir::createCSEPass());
  pm->addPass(mlir::createLoopInvariantCodeMotionPass());
  pm->addPass(mlir::createCanonicalizerPass());

  if (cc.has_amd_matrix_core()) {
    pm->addPass(
        mlir::createTritonAMDGPUStreamPipeline({num_stages, 0, 0, false}));
    // TODO(ROCm) Modify when corresponding run time flags are introduced.
    if (/*use_async_copy=*/false) {  // Not enabled by default.
      pm->addPass(mlir::createTritonAMDGPUCoalesceAsyncCopy());
    }
    pm->addPass(mlir::createCanonicalizerPass());
  }
  if (/*(instruction_sched_variant=="none") == */ false) {
    pm->addPass(mt::createTritonAMDGPUInsertInstructionSchedHintsPass("none"));
  }
  pm->addPass(mt::gpu::createTritonGPUOptimizeDotOperands({true}));
  pm->addPass(mt::gpu::createTritonGPURemoveLayoutConversions());
  pm->addPass(mt::gpu::createTritonGPUReduceDataDuplication());
  if (/*(instruction_sched_variant=="none") == */ false) {
    pm->addPass(mlir::createTritonAMDGPUInThreadTranspose());
    pm->addPass(mt::gpu::createTritonGPURemoveLayoutConversions());
  }
  if (cc.has_amd_matrix_core()) {
    pm->addPass(mt::gpu::createTritonGPUReorderInstructions());
  }
  if (/*(use_block_pingpong == "none") ==*/false) {
    pm->addPass(mlir::createTritonAMDGPUBlockPingpong({num_stages}));
  }
  if (/*use_buffer_ops=*/false) {  // Not enabled by default.
    pm->addPass(mlir::createTritonAMDGPUCanonicalizePointers());
    pm->addPass(mlir::createCanonicalizerPass());
    pm->addPass(mlir::createTritonAMDGPUConvertToBufferOps({arch_name}));
  }
  pm->addPass(mlir::createTritonAMDFoldTrueCmpI());
  pm->addPass(mlir::createCanonicalizerPass());
  pm->addPass(mlir::createCSEPass());
  pm->addPass(mlir::createSymbolDCEPass());

  // Based on make_llir() in
  // @triton//:third_party/amd/backend/compiler.py
  const int custom_lds_size = 0;
  pm->addPass(mlir::triton::AMD::createOptimizeLDSUsagePass(cc.gfx_version(),
                                                            custom_lds_size));
  pm->addPass(mlir::createSCFToControlFlowPass());
  pm->addPass(mlir::createConvertIndexToLLVMPass());
  pm->addPass(mt::gpu::createAllocateSharedMemory());
  pm->addPass(mt::createConvertTritonAMDGPUToLLVMPass(cc.gfx_version(), true));
  pm->addPass(mlir::createCanonicalizerPass());
  pm->addPass(mlir::createCSEPass());
  // Note: translateTritonGPUToLLVMIR adds line info with LLVMDIScopePass.
  pm->addPass(mlir::createConvertControlFlowToLLVMPass());
  pm->addPass(mlir::createArithToLLVMConversionPass());
  pm->addPass(mlir::createCanonicalizerPass());
  pm->addPass(mlir::createCSEPass());
  pm->addPass(mlir::createSymbolDCEPass());
  if (/*(instruction_sched_variant=="none") == */ false) {
    pm->addPass(mt::createTritonAMDGPULowerInstructionSchedHintsPass(
        cc.gfx_version(), num_stages));
  }
  pm->addPass(mt::createConvertBuiltinFuncToLLVMPass(/*ftz=*/true));
  // There is no clusters in ROCm for now.
  out_cluster_info.clusterDimX = 1;
  out_cluster_info.clusterDimY = 1;
  out_cluster_info.clusterDimZ = 1;

  return absl::OkStatus();
}

std::string GetLibdevicePath(const HloModuleConfig& hlo_config,
                             const se::DeviceDescription& device_info) {
  std::string libdevice_dir = tsl::RocdlRoot();
  auto compute_capability = device_info.rocm_compute_capability();
  const std::string libdevice_path =
      amdgpu::LibDevicePath(compute_capability.gcn_arch_name(), libdevice_dir);
  return libdevice_path;
}

}  // namespace gpu
}  // namespace xla

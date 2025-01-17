/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/xla/service/gpu/parallel_loop_emitter.h"

#include <memory>

#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/types.h"
// IWYU pragma: no_include "llvm/IR/Intrinsics.gen.inc"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Value.h"
#include "tensorflow/compiler/xla/service/gpu/target_util.h"
#include "tensorflow/compiler/xla/service/llvm_ir/kernel_support_library.h"
#include "tensorflow/compiler/xla/service/llvm_ir/llvm_loop.h"
#include "tensorflow/compiler/xla/service/llvm_ir/llvm_util.h"
#include "tensorflow/compiler/xla/shape_util.h"

namespace xla {
namespace gpu {

ParallelLoopEmitter::ParallelLoopEmitter(
    BodyEmitter body_emitter, const Shape& shape,
    const LaunchDimensions& launch_dimensions, llvm::IRBuilder<>* b,
    int unroll_factor)
    : LoopEmitter(body_emitter, shape, b),
      launch_dimensions_(launch_dimensions),
      unroll_factor_(unroll_factor) {}

ParallelLoopEmitter::ParallelLoopEmitter(
    const llvm_ir::ElementGenerator& target_element_generator,
    absl::Span<const llvm_ir::IrArray> target_arrays,
    const LaunchDimensions& launch_dimensions, llvm::IRBuilder<>* b,
    int unroll_factor)
    : LoopEmitter(target_element_generator, target_arrays, b),
      launch_dimensions_(launch_dimensions),
      unroll_factor_(unroll_factor) {}

ParallelLoopEmitter::ParallelLoopEmitter(
    const llvm_ir::ElementGenerator& target_element_generator,
    const llvm_ir::IrArray& target_array,
    const LaunchDimensions& launch_dimensions, llvm::IRBuilder<>* b,
    int unroll_factor)
    : LoopEmitter(target_element_generator, target_array, b),
      launch_dimensions_(launch_dimensions),
      unroll_factor_(unroll_factor) {}

std::vector<llvm_ir::IrArray::Index>
ParallelLoopEmitter::EmitIndexAndSetExitBasicBlock(absl::string_view loop_name,
                                                   llvm::Type* index_type,
                                                   llvm::Value* base_index) {
  // Emit the following code in LLVM IR:
  //   linear_index = blockIdx.x * blockDim.x + threadIdx.x;
  //   if (linear_index < num_elements) {
  //     array_index = LinearIndexToMultidimensionalIndex(shape_, linear_index);
  //     ...
  //   }

  // Per the PTX documentation:
  //   "It is guaranteed that [...] 0  <=  %ctaid.x <  %nctaid.x"
  //
  // %nctaid.x is currently specified as 2147483647.
  VLOG(3) << "EmitIndexAndSetExitBasicBlock unroll_factor " << unroll_factor_;
  CHECK_NE(index_type, nullptr);
  std::vector<llvm_ir::IrArray::Index> array_indices;
  llvm::Value* block_id =
      EmitCallToTargetIntrinsic(TargetIntrinsicID::kBlockIdx, {}, {}, b_);
  llvm_ir::AddRangeMetadata(0, launch_dimensions_.block_counts().x,
                            static_cast<llvm::Instruction*>(block_id));
  block_id = b_->CreateZExtOrTrunc(block_id, index_type, "block_id");

  // Per the PTX documentation:
  //   "It is guaranteed that [...] 0  <=  %tid.x <  %ntid.x"
  //
  // %ntid.x is currently specified as 1024.
  llvm::Value* thread_id =
      EmitCallToTargetIntrinsic(TargetIntrinsicID::kThreadIdx, {}, {}, b_);
  llvm_ir::AddRangeMetadata(0, launch_dimensions_.thread_counts_per_block().x,
                            static_cast<llvm::Instruction*>(thread_id));
  thread_id = b_->CreateZExtOrTrunc(thread_id, index_type, "thread_id");

  llvm::Value* linear_index_base = b_->CreateAdd(
      b_->CreateMul(
          block_id,
          llvm::ConstantInt::get(
              index_type, launch_dimensions_.thread_counts_per_block().x),
          "",
          /*HasNUW=*/true, /*HasNSW=*/true),
      thread_id, "linear_index", /*HasNUW=*/true, /*HasNSW=*/true);

  // Add an @llvm.assume(linear_index < threads_per_block * num_blocks).
  //
  // This might seem obvious from the computation above, but LLVM does not
  // currently determine the range of linear_index precisely.  InstCombine uses
  // known-bits, which, when applied to the task of determining a value's range,
  // is imprecise for everything other than powers of 2.  And
  // CorrelatedValuePropagation is, as a cost-saving measure, disabled for
  // conditions in the same basic block as their operands.
  llvm_ir::EmitCallToIntrinsic(
      llvm::Intrinsic::assume,
      {b_->CreateICmpULT(
          linear_index_base,
          llvm::ConstantInt::get(
              index_type, launch_dimensions_.thread_counts_per_block().x *
                              launch_dimensions_.block_counts().x),
          "linear_index_in_range")},
      {}, b_);

  if (unroll_factor_ > 1) {
    linear_index_base = b_->CreateMul(
        linear_index_base, llvm::ConstantInt::get(index_type, unroll_factor_),
        "linear_index_base", /*HasNUW=*/true, /*HasNSW=*/true);
  }

  if (base_index != nullptr) {
    linear_index_base =
        b_->CreateAdd(linear_index_base, base_index, "linear_index_plus_base",
                      /*HasNUW=*/true, /*HasNSW=*/true);
  }

  // When enable_row_index is true, it means the inner most dimensions
  // match the block sizes.  So we can generate a simpler indexing
  // for that dimensions.  This helps LLVM generate vectorized codes
  // in that cases.
  LaunchDimensions::Dim3D dim3 = launch_dimensions_.thread_counts_per_block();
  bool enable_row_index =
      shape_.rank() > 1 && unroll_factor_ > 1 && shape_.has_layout() &&
      shape_.layout().minor_to_major()[shape_.rank() - 1] == 0 &&
      dim3.x * dim3.y * dim3.z * unroll_factor_ == shape_.dimensions().back();
  VLOG(2) << "Emitting row optimized indexing: " << enable_row_index;
  llvm::Value* row_index = nullptr;
  if (!enable_row_index) {
    array_indices.emplace_back(linear_index_base, shape_, b_);
  } else {
    // Simpler index for row computation.
    // This will allow LLVM to vectorize.
    row_index = b_->CreateMul(
        thread_id, llvm::ConstantInt::get(index_type, unroll_factor_),
        "row_index", /*HasNUW=*/true, /*HasNSW=*/true);
    std::vector<llvm::Value*> multidim(shape_.rank(), nullptr);
    multidim.back() = row_index;
    array_indices.emplace_back(linear_index_base, multidim, shape_, b_);
  }

  for (int i = 1; i < unroll_factor_; ++i) {
    llvm::Value* linear_index =
        b_->CreateAdd(linear_index_base, llvm::ConstantInt::get(index_type, i),
                      absl::StrCat("linear_index", i),
                      /*HasNUW=*/true, /*HasNSW=*/true);
    if (!enable_row_index) {
      array_indices.emplace_back(linear_index, shape_, b_);
    } else {
      std::vector<llvm::Value*> multidim(shape_.rank(), nullptr);
      multidim.back() = b_->CreateAdd(
          row_index, llvm::ConstantInt::get(index_type, i),
          absl::StrCat("row_index_plus", i), /*HasNUW=*/true, /*HasNSW=*/true);
      array_indices.emplace_back(linear_index, multidim, shape_, b_);
    }
  }

  auto if_in_bounds = llvm_ir::EmitIfThenElse(
      b_->CreateICmpULT(
          linear_index_base,
          llvm::ConstantInt::get(index_type, ShapeUtil::ElementsIn(shape_))),
      llvm_ir::IrName(loop_name, "in_bounds"), b_, false);

  // Set exit_bb_ to the exit block of the if structure.
  exit_bb_ = if_in_bounds.after_block;
  CHECK_NE(nullptr, exit_bb_);

  // Set IR builder insertion point to the body of the if structure.
  llvm_ir::SetToFirstInsertPoint(if_in_bounds.true_block, b_);

  return array_indices;
}

Status ParallelLoopEmitter::EmitLoop(absl::string_view loop_name,
                                     llvm::Type* index_type) {
  if (index_type == nullptr) {
    index_type = b_->getInt64Ty();
  }
  int64 total_threads = launch_dimensions_.launch_bound();
  int64 num_elements = ShapeUtil::ElementsIn(shape_);
  // If all the elements are handled by the current threads, no need
  // to add a loop inside the kernel.
  if (total_threads * unroll_factor_ >= num_elements) {
    VLOG(1) << "ParallelLoopEmitter::EmitLoop fallback";
    return LoopEmitter::EmitLoop(loop_name, index_type);
  }

  KernelSupportLibrary ksl(b_, llvm_ir::UnrollMode::kDefaultUnroll);
  auto constant = [&](int64 val) {
    return llvm::ConstantInt::get(index_type, val);
  };

  TF_RETURN_IF_ERROR(ksl.ForWithStatus(
      "loop", constant(0), constant(num_elements),
      constant(total_threads * unroll_factor_), [&](llvm::Value* base_indvar) {
        for (const llvm_ir::IrArray::Index& array_index :
             EmitIndexAndSetExitBasicBlock(loop_name, index_type,
                                           base_indvar)) {
          TF_RETURN_IF_ERROR(body_emitter_(array_index));
        }
        return Status::OK();
      }));

  // Set the insertion point of b_ to the loop exit, so that
  // code emitted for later instructions will be correctly placed.
  if (exit_bb_ != nullptr) {
    b_->SetInsertPoint(exit_bb_);
  }
  return Status::OK();
}

}  // namespace gpu
}  // namespace xla

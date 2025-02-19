// Copyright 2023 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree-dialects/Dialect/LinalgExt/IR/LinalgExtDialect.h"
#include "iree-dialects/Dialect/LinalgExt/IR/LinalgExtOps.h"
#include "iree-dialects/Dialect/LinalgExt/Passes/Passes.h"
#include "iree-dialects/Dialect/LinalgExt/Utils/Utils.h"
#include "iree/compiler/Codegen/Common/CPU/PassDetail.h"
#include "iree/compiler/Codegen/Common/CPU/Passes.h"
#include "iree/compiler/Codegen/Common/EncodingInfo.h"
#include "iree/compiler/Codegen/Dialect/IREECodegenDialect.h"
#include "iree/compiler/Codegen/Utils/Utils.h"
#include "iree/compiler/Dialect/HAL/IR/HALTypes.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/MemRef/Transforms/Transforms.h"
#include "mlir/IR/BuiltinTypeInterfaces.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/Transforms/DialectConversion.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"

namespace mlir {
namespace iree_compiler {

using namespace IREE::LinalgExt;
using IREE::HAL::ExecutableTargetAttr;

namespace {

static MatmulTileParams
chooseMatmulTileParamsGeneric(ExecutableTargetAttr target) {
  if (isVMVXBackend(target) && hasMicrokernels(target)) {
    // VMVX+ukernel uses dynamic tile shapes.
    return {ShapedType::kDynamic, ShapedType::kDynamic, ShapedType::kDynamic};
  } else {
    // Some vaguely reasonable static tile shape.
    return {8, 4, 8};
  }
}

static MatmulTileParams
chooseMatmulTileParamsAArch64(EncodingUser user, ExecutableTargetAttr target) {
  switch (user) {
  case EncodingUser::MATMUL_F32F32F32:
  case EncodingUser::MATMUL_F16F16F32:
  case EncodingUser::MATMUL_F16F16F16:
  case EncodingUser::MATMUL_BF16BF16F32:
  case EncodingUser::MATMUL_BF16BF16BF16:
  case EncodingUser::BATCH_MATMUL_F32F32F32:
  case EncodingUser::BATCH_MATMUL_F16F16F32:
  case EncodingUser::BATCH_MATMUL_F16F16F16:
  case EncodingUser::BATCH_MATMUL_BF16BF16F32:
  case EncodingUser::BATCH_MATMUL_BF16BF16BF16:
    // Note: 16-bit floating point types currently use the same tile size as
    // f32. This makes sense when either (1) the accumulator is f32, or (2)
    // the arithmetic will have to expand f16 to f32 in registers. We may
    // reconsider when taking advantage of native f16/bf16 arithmetic when the
    // accumulator itself is f16/bf16.
    return {8, 1, 8};
  case EncodingUser::MATMUL_I8I8I32:
  case EncodingUser::BATCH_MATMUL_I8I8I32:
    if (hasFeature(target, "+i8mm")) {
      // Aim to use SMMLA.
      return {8, 8, 8};
    }
    if (hasFeature(target, "+dotprod")) {
      // Aim to use SDOT.
      return {8, 4, 8};
    }
    return {8, 1, 8};
  default:
    assert(false);
    return {};
  }
}

static MatmulTileParams
chooseMatmulTileParamsX86_64(EncodingUser user, ExecutableTargetAttr target) {
  switch (user) {
  case EncodingUser::MATMUL_F32F32F32:
  case EncodingUser::MATMUL_F16F16F32:
  case EncodingUser::MATMUL_F16F16F16:
  case EncodingUser::MATMUL_BF16BF16F32:
  case EncodingUser::MATMUL_BF16BF16BF16:
  case EncodingUser::BATCH_MATMUL_F32F32F32:
  case EncodingUser::BATCH_MATMUL_F16F16F32:
  case EncodingUser::BATCH_MATMUL_F16F16F16:
  case EncodingUser::BATCH_MATMUL_BF16BF16F32:
  case EncodingUser::BATCH_MATMUL_BF16BF16BF16:
    // Note: 16-bit floating point types currently use the same tile size as
    // f32. This makes sense when either (1) the accumulator is f32, or (2)
    // the arithmetic will have to expand f16 to f32 in registers. We may
    // reconsider when taking advantage of native f16/bf16 arithmetic when the
    // accumulator itself is f16/bf16.
    if (hasFeature(target, "+avx512f")) {
      return {16, 1, 16};
    }
    if (hasFeature(target, "+avx")) {
      // Note: for good performance, most +avx users will also want to add
      // +fma, but that's a local instruction selection detail and the tile
      // layout is unaffected, as there are enough registers even with the
      // need for intermediate product registers when +fma is not used.
      return {8, 1, 8};
    }
    // SSE fallback.
    return {8, 1, 4};
  case EncodingUser::MATMUL_I8I8I32:
  case EncodingUser::BATCH_MATMUL_I8I8I32:
    if (hasFeature(target, "+avx512vnni")) {
      // Aim to use VPDPWSSD. This is the same tile size as with VPMADDWD
      // as the only difference is that VPDPWSSD accumulates. VPDPBUSD would
      // call for {16, 4, 16} but we can't use it because of its unsigned LHS.
      return {16, 2, 16};
    }
    if (hasFeature(target, "+avx512bw")) {
      // Aim to use VPMADDWD (zmm).
      return {16, 2, 16};
    }
    if (hasFeature(target, "+avx2")) {
      // Aim to use VPMADDWD (ymm).
      return {8, 2, 8};
    }
    // SSE fallback. Aim to use PMADDWD (xmm).
    return {8, 2, 4};
  default:
    assert(false);
    return {};
  }
}

static MatmulTileParams chooseMatmulTileParams(EncodingUser user,
                                               ExecutableTargetAttr target) {
  if (isAArch64(target)) {
    return chooseMatmulTileParamsAArch64(user, target);
  }
  if (isX86_64(target)) {
    return chooseMatmulTileParamsX86_64(user, target);
  }
  return chooseMatmulTileParamsGeneric(target);
}

struct CPUMaterializeEncodingPass
    : public CPUMaterializeEncodingBase<CPUMaterializeEncodingPass> {
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<arith::ArithDialect, IREE::LinalgExt::IREELinalgExtDialect,
                    IREE::Codegen::IREECodegenDialect>();
  }
  void runOnOperation() override;
};

struct CPUMaterializeUpperBoundTileSizePass
    : public CPUMaterializeUpperBoundTileSizeBase<
          CPUMaterializeUpperBoundTileSizePass> {
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<arith::ArithDialect>();
  }
  void runOnOperation() override;
};

FailureOr<MaterializeEncodingInfo>
materializeEncodingForTarget(RankedTensorType tensorType,
                             ExecutableTargetAttr targetAttr) {
  IREE::LinalgExt::EncodingAttr encoding =
      tensorType.getEncoding()
          .dyn_cast_or_null<IREE::LinalgExt::EncodingAttr>();
  if (!encoding) {
    return failure();
  }
  auto user = encoding.getUser().getValue();
  auto role = encoding.getRole().getValue();
  MatmulTileParams tileParams = chooseMatmulTileParams(user, targetAttr);
  auto encodingInfo = chooseEncodingInfoForMatmul(user, role, tileParams);
  auto originalTypeAttr = encoding.getOriginalType();
  auto originalType = originalTypeAttr
                          ? originalTypeAttr.getValue().cast<RankedTensorType>()
                          : tensorType;
  // TODO(bjacob): not sure why this causes buffer issues with VMVX.
  if (!isVMVXBackend(targetAttr)) {
    adjustTileSizesToNarrowStaticShape(encodingInfo, originalType.getShape());
  }
  return encodingInfo;
}

MaterializeEncodingFn
getMaterializeEncodingFn(ExecutableTargetAttr targetAttr) {
  return
      [targetAttr](
          RankedTensorType tensorType) -> FailureOr<MaterializeEncodingInfo> {
        return materializeEncodingForTarget(tensorType, targetAttr);
      };
}

// Like getMaterializeEncodingFn, but iterating over an array of targets and
// returning the max of all tile sizes from each target, checking that other
// materialization info (permutations) agree.
//
// This is useful to compute padding amounts, in the materialization of
// UpperBoundTileSizeOp, in top-level functions that are not part of one HAL
// executable variant. There, the padding amounts only control the size of
// allocated buffers, so it's OK to over-estimate (only wasting some memory)
// but not under-estimate (would cause buffer overruns) padding amounts.
MaterializeEncodingFn
getUpperBoundMaterializeEncodingFn(ArrayRef<ExecutableTargetAttr> targetAttrs) {
  return
      [targetAttrs](
          RankedTensorType tensorType) -> FailureOr<MaterializeEncodingInfo> {
        FailureOr<MaterializeEncodingInfo> result; // Defaults to failure.
        for (auto targetAttr : targetAttrs) {
          FailureOr<MaterializeEncodingInfo> info =
              materializeEncodingForTarget(tensorType, targetAttr);
          if (failed(info)) {
            // No info at this iteration. Ignore and continue.
            continue;
          }
          if (failed(result)) {
            // No preexisting result. Use this iteration's info and continue.
            result = info;
            continue;
          }
          // Merge this iteration's info into preexisting result info.
          // Check that permutations match, then record the max of tile sizes.
          if (info->innerDimsPos != result->innerDimsPos ||
              info->outerDimsPerm != result->outerDimsPerm) {
            return failure();
          }
          if (info->innerTileSizes.size() != result->innerTileSizes.size()) {
            return failure();
          }
          for (unsigned i = 0; i < info->innerTileSizes.size(); ++i) {
            if (info->innerTileSizes[i] == ShapedType::kDynamic) {
              result->innerTileSizes[i] = ShapedType::kDynamic;
            } else {
              result->innerTileSizes[i] =
                  std::max(result->innerTileSizes[i], info->innerTileSizes[i]);
            }
          }
        }
        return result;
      };
}

} // namespace

void CPUMaterializeEncodingPass::runOnOperation() {
  MLIRContext *context = &getContext();
  auto operation = getOperation();
  RewritePatternSet materializeEncodingPattern(context);
  auto targetAttr = ExecutableTargetAttr::lookup(operation);
  auto materializeEncodingFn = getMaterializeEncodingFn(targetAttr);
  if (!materializeEncodingFn) {
    return signalPassFailure();
  }
  MaterializeEncodingTypeConverter typeConverter(materializeEncodingFn);
  MaterializeEncodingConversionTarget target(*context);
  auto materializeEncodingValueFn = getMaterializeEncodingValueFn(targetAttr);
  populateMaterializeEncodingIntoPackUnPackPatterns(materializeEncodingPattern,
                                                    target, typeConverter,
                                                    materializeEncodingValueFn);

  if (failed(applyPartialConversion(operation, target,
                                    std::move(materializeEncodingPattern)))) {
    operation.emitOpError("materialization failed");
    return signalPassFailure();
  }

  // Add patterns to fold pack/unpack ops with pad/extract_slice ops and resolve
  // dims ops.
  {
    RewritePatternSet patterns(context);
    tensor::populateFoldIntoPackAndUnpackPatterns(patterns);
    memref::populateResolveRankedShapedTypeResultDimsPatterns(patterns);
    if (failed(applyPatternsAndFoldGreedily(operation, std::move(patterns)))) {
      operation.emitOpError("folding patterns failed");
      return signalPassFailure();
    }
  }
}

void CPUMaterializeUpperBoundTileSizePass::runOnOperation() {
  MLIRContext *context = &getContext();
  auto operation = getOperation();
  auto targetAttrs =
      IREE::HAL::DeviceTargetAttr::lookupExecutableTargets(operation);
  RewritePatternSet patterns(context);
  MaterializeEncodingFn materializeEncodingFn =
      getUpperBoundMaterializeEncodingFn(targetAttrs);
  if (!materializeEncodingFn) {
    return signalPassFailure();
  }
  populateMaterializeUpperBoundTileSizePatterns(patterns,
                                                materializeEncodingFn);
  if (failed(applyPatternsAndFoldGreedily(operation, std::move(patterns)))) {
    operation.emitOpError(
        "encoding padding sizes materialization pattern failed");
    return signalPassFailure();
  }
}

std::unique_ptr<OperationPass<func::FuncOp>>
createCPUMaterializeEncodingPass() {
  return std::make_unique<CPUMaterializeEncodingPass>();
}

std::unique_ptr<InterfacePass<mlir::FunctionOpInterface>>
createCPUMaterializeUpperBoundTileSizePass() {
  return std::make_unique<CPUMaterializeUpperBoundTileSizePass>();
}

} // namespace iree_compiler
} // namespace mlir

//===--------------------------------------------------------------------------------*- C++ -*-===//
//                          _
//                         | |
//                       __| | __ ___      ___ ___
//                      / _` |/ _` \ \ /\ / / '_  |
//                     | (_| | (_| |\ V  V /| | | |
//                      \__,_|\__,_| \_/\_/ |_| |_| - Compiler Toolchain
//
//
//  This file is distributed under the MIT License (MIT).
//  See LICENSE.txt for details.
//
//===------------------------------------------------------------------------------------------===//

#include "dawn/Compiler/DawnCompiler.h"
#include "dawn/CodeGen/CXXNaive-ico/CXXNaiveCodeGen.h"
#include "dawn/CodeGen/CXXNaive/CXXNaiveCodeGen.h"
#include "dawn/CodeGen/CodeGen.h"
#include "dawn/CodeGen/Cuda/CudaCodeGen.h"
#include "dawn/CodeGen/GridTools/GTCodeGen.h"
#include "dawn/Optimizer/OptimizerContext.h"
#include "dawn/Optimizer/PassComputeStageExtents.h"
#include "dawn/Optimizer/PassDataLocalityMetric.h"
#include "dawn/Optimizer/PassFieldVersioning.h"
#include "dawn/Optimizer/PassFixVersionedInputFields.h"
#include "dawn/Optimizer/PassInlining.h"
#include "dawn/Optimizer/PassIntervalPartitioning.h"
#include "dawn/Optimizer/PassLocalVarType.h"
#include "dawn/Optimizer/PassMultiStageSplitter.h"
#include "dawn/Optimizer/PassPrintStencilGraph.h"
#include "dawn/Optimizer/PassSSA.h"
#include "dawn/Optimizer/PassSetBlockSize.h"
#include "dawn/Optimizer/PassSetBoundaryCondition.h"
#include "dawn/Optimizer/PassSetCaches.h"
#include "dawn/Optimizer/PassSetDependencyGraph.h"
#include "dawn/Optimizer/PassSetNonTempCaches.h"
#include "dawn/Optimizer/PassSetStageGraph.h"
#include "dawn/Optimizer/PassSetStageName.h"
#include "dawn/Optimizer/PassSetSyncStage.h"
#include "dawn/Optimizer/PassStageMerger.h"
#include "dawn/Optimizer/PassStageReordering.h"
#include "dawn/Optimizer/PassStageSplitter.h"
#include "dawn/Optimizer/PassStencilSplitter.h"
#include "dawn/Optimizer/PassTemporaryFirstAccess.h"
#include "dawn/Optimizer/PassTemporaryMerger.h"
#include "dawn/Optimizer/PassTemporaryToStencilFunction.h"
#include "dawn/Optimizer/PassTemporaryType.h"
#include "dawn/Optimizer/PassValidation.h"
#include "dawn/SIR/SIR.h"
#include "dawn/Serialization/IIRSerializer.h"
#include "dawn/Support/Array.h"
#include "dawn/Support/EditDistance.h"
#include "dawn/Support/Exception.h"
#include "dawn/Support/FileSystem.h"
#include "dawn/Support/Logging.h"
#include "dawn/Support/StringSwitch.h"
#include "dawn/Support/StringUtil.h"
#include "dawn/Support/Unreachable.h"

namespace dawn {

namespace {

// CodeGen backends
enum class BackendType { GridTools, CXXNaive, CXXNaiveIco, CUDA, CXXOpt };

BackendType parseBackendString(const std::string& backendStr) {
  if(backendStr == "gt" || backendStr == "gridtools") {
    return BackendType::GridTools;
  } else if(backendStr == "naive" || backendStr == "cxxnaive" || backendStr == "c++-naive") {
    return BackendType::CXXNaive;
  } else if(backendStr == "ico" || backendStr == "naive-ico" || backendStr == "c++-naive-ico") {
    return BackendType::CXXNaiveIco;
  } else if(backendStr == "cuda" || backendStr == "CUDA") {
    return BackendType::CUDA;
  } else {
    throw CompileError("Backend not supported");
  }
}

/// @brief Make a suggestion to the user if there is a small typo (only works with string options)
template <class T>
struct ComputeEditDistance {
  static std::string getSuggestion(const T& value, const std::vector<T>& possibleValues) {
    return "";
  }
};

template <>
struct ComputeEditDistance<std::string> {
  static std::string getSuggestion(const std::string& value,
                                   const std::vector<std::string>& possibleValues) {
    if(possibleValues.empty())
      return "";

    std::vector<unsigned> editDistances;
    std::transform(possibleValues.begin(), possibleValues.end(), std::back_inserter(editDistances),
                   [&](const std::string& val) { return computeEditDistance(value, val); });

    // Find minimum edit distance
    unsigned minEditDistance = editDistances[0], minEditDistanceIdx = 0;
    for(unsigned i = 1; i < editDistances.size(); ++i)
      if(editDistances[i] < minEditDistance) {
        minEditDistance = editDistances[i];
        minEditDistanceIdx = i;
      }

    return minEditDistance <= 2 ? "did you mean '" + possibleValues[minEditDistanceIdx] + "' ?"
                                : "";
  }
};

OptimizerContext::OptimizerContextOptions
createOptimizerOptionsFromAllOptions(const Options& options) {
  OptimizerContext::OptimizerContextOptions retval;
#define OPT(TYPE, NAME, DEFAULT_VALUE, OPTION, OPTION_SHORT, HELP, VALUE_NAME, HAS_VALUE, F_GROUP) \
  retval.NAME = options.NAME;
#include "dawn/Optimizer/OptimizerOptions.inc"
#undef OPT
  return retval;
}

/// @brief Report a diagnostic concering an invalid Option
template <class T>
DiagnosticsBuilder buildDiag(const std::string& option, const T& value, std::string reason,
                             std::vector<T> possibleValues = std::vector<T>{}) {
  DiagnosticsBuilder diag(DiagnosticsKind::Error, SourceLocation());
  diag << "invalid value '" << value << "' of option '" << option << "'";

  if(!reason.empty()) {
    diag << ", " << reason;
  } else {
    auto suggestion = ComputeEditDistance<T>::getSuggestion(value, possibleValues);

    if(!suggestion.empty())
      diag << ", " << suggestion;
    else if(!possibleValues.empty())
      diag << ", possible values " << RangeToString()(possibleValues);
  }
  return diag;
}

static bool shouldRunPass(const Options& options, bool runSpecificPass) {
  return !options.DefaultNone || runSpecificPass;
}

} // namespace

DawnCompiler::DawnCompiler(const Options& options) : diagnostics_(), options_(options) {}

std::map<std::string, std::shared_ptr<iir::StencilInstantiation>>
DawnCompiler::lowerToIIR(std::shared_ptr<SIR> const& stencilIR) {
  diagnostics_.clear();
  diagnostics_.setFilename(stencilIR->Filename);

  // -reorder
  using ReorderStrategyKind = ReorderStrategy::Kind;
  ReorderStrategyKind reorderStrategy = StringSwitch<ReorderStrategyKind>(options_.ReorderStrategy)
                                            .Case("none", ReorderStrategyKind::None)
                                            .Case("greedy", ReorderStrategyKind::Greedy)
                                            .Case("scut", ReorderStrategyKind::Partitioning)
                                            .Default(ReorderStrategyKind::Unknown);

  if(reorderStrategy == ReorderStrategyKind::Unknown) {
    diagnostics_.report(
        buildDiag("-reorder", options_.ReorderStrategy, "", {"none", "greedy", "scut"}));
    throw std::runtime_error("An error occurred.");
  }

  using MultistageSplitStrategy = PassMultiStageSplitter::MultiStageSplittingStrategy;
  MultistageSplitStrategy mssSplitStrategy;
  if(options_.MaxCutMSS) {
    mssSplitStrategy = MultistageSplitStrategy::MaxCut;
  } else {
    mssSplitStrategy = MultistageSplitStrategy::Optimized;
  }

  // Initialize optimizer
  OptimizerContext optimizer(getDiagnostics(), createOptimizerOptionsFromAllOptions(options_),
                             stencilIR);

  if(shouldRunPass(options_, options_.Parallel)) {
    // required passes to have proper, parallelized IR
    optimizer.checkAndPushBack<PassInlining>(true, PassInlining::InlineStrategy::InlineProcedures);
    optimizer.checkAndPushBack<PassFieldVersioning>();
    optimizer.checkAndPushBack<PassMultiStageSplitter>(mssSplitStrategy);
    optimizer.checkAndPushBack<PassStageSplitter>();
    optimizer.checkAndPushBack<PassTemporaryType>();
    optimizer.checkAndPushBack<PassFixVersionedInputFields>();
    optimizer.checkAndPushBack<PassComputeStageExtents>();
    optimizer.checkAndPushBack<PassSetSyncStage>();
    // validation checks after parallelisation
    optimizer.checkAndPushBack<PassLocalVarType>();
    optimizer.checkAndPushBack<PassValidation>();
  }

  DAWN_LOG(INFO) << "All the passes ran with the current command line arguments:";
  for(const auto& a : optimizer.getPassManager().getPasses()) {
    DAWN_LOG(INFO) << a->getName();
  }

  for(auto& stencil : optimizer.getStencilInstantiationMap()) {
    // Run optimization passes
    std::shared_ptr<iir::StencilInstantiation> instantiation = stencil.second;

    DAWN_LOG(INFO) << "Starting parallelisation passes for `" << instantiation->getName()
                   << "` ...";
    if(!optimizer.getPassManager().runAllPassesOnStencilInstantiation(optimizer, instantiation))
      throw std::runtime_error("An error occurred.");

    DAWN_LOG(INFO) << "Done with parallelisation passes for `" << instantiation->getName() << "`";
  }

  auto stencilInstantiationMap = optimizer.getStencilInstantiationMap();
  return stencilInstantiationMap;
}

std::map<std::string, std::shared_ptr<iir::StencilInstantiation>>
DawnCompiler::optimize(std::map<std::string, std::shared_ptr<iir::StencilInstantiation>> const&
                           stencilInstantiationMap) {
  // -reorder
  using ReorderStrategyKind = ReorderStrategy::Kind;
  ReorderStrategyKind reorderStrategy = StringSwitch<ReorderStrategyKind>(options_.ReorderStrategy)
                                            .Case("none", ReorderStrategyKind::None)
                                            .Case("greedy", ReorderStrategyKind::Greedy)
                                            .Case("scut", ReorderStrategyKind::Partitioning)
                                            .Default(ReorderStrategyKind::Unknown);

  if(reorderStrategy == ReorderStrategyKind::Unknown) {
    diagnostics_.report(
        buildDiag("-reorder", options_.ReorderStrategy, "", {"none", "greedy", "scut"}));
    throw std::runtime_error("An error occurred.");
  }

  IIRSerializer::Format serializationKind = IIRSerializer::Format::Json;
  if(options_.SerializeIIR || (options_.DeserializeIIR != "")) {
    if(options_.IIRFormat == "json") {
      serializationKind = IIRSerializer::Format::Json;
    } else if(options_.IIRFormat == "byte") {
      serializationKind = IIRSerializer::Format::Byte;
    } else {
      dawn_unreachable("Unknown SIRFormat option");
    }
  }

  // Initialize optimizer
  OptimizerContext optimizer(getDiagnostics(), createOptimizerOptionsFromAllOptions(options_),
                             stencilInstantiationMap);

  // Optimization, step by step
  //===-----------------------------------------------------------------------------------------
  // if(shouldRunPass(options_, options_.SSA)) {
  //   // broken but should run with no prerequesits
  //   optimizer.checkAndPushBack<PassSSA>();
  //   // rerun things we might have changed
  //   // optimizer.checkAndPushBack<PassFixVersionedInputFields>();
  //   // todo: this does not work since it does not check if it was already run
  // }
  //===-----------------------------------------------------------------------------------------
  if(shouldRunPass(options_, options_.PrintStencilGraph)) {
    // --print-stencil-graph
    // wow
    optimizer.checkAndPushBack<PassSetDependencyGraph>();
    // Plain diagnostics, should not even be a pass but is independent
    optimizer.checkAndPushBack<PassPrintStencilGraph>();
    // validation check
    optimizer.checkAndPushBack<PassValidation>();
  }
  //===-----------------------------------------------------------------------------------------
  if(shouldRunPass(options_, options_.SetStageName)) {
    // This is never used but if we want to reenable it, it is independent
    optimizer.checkAndPushBack<PassSetStageName>();
    // validation check
    optimizer.checkAndPushBack<PassValidation>();
  }
  //===-----------------------------------------------------------------------------------------
  if(shouldRunPass(options_, options_.ReorderStages)) {
    optimizer.checkAndPushBack<PassSetStageGraph>();
    optimizer.checkAndPushBack<PassSetDependencyGraph>();
    optimizer.checkAndPushBack<PassStageReordering>(reorderStrategy);
    // moved stages around ...
    optimizer.checkAndPushBack<PassSetSyncStage>();
    // if we want this info around, we should probably run this also
    // optimizer.checkAndPushBack<PassSetStageName>();
    // validation check
    optimizer.checkAndPushBack<PassValidation>();
  }
  //===-----------------------------------------------------------------------------------------
  if(shouldRunPass(options_, options_.MergeStages)) {
    // merging requires the stage graph
    optimizer.checkAndPushBack<PassSetStageGraph>();
    // running the actual pass
    optimizer.checkAndPushBack<PassStageMerger>();
    // since this can change the scope of temporaries ...
    optimizer.checkAndPushBack<PassTemporaryType>();
    optimizer.checkAndPushBack<PassLocalVarType>();
    // this pass is broken as hell... Johann please
    // optimizer.checkAndPushBack<PassFixVersionedInputFields>();
    // modify stages and their extents ...
    optimizer.checkAndPushBack<PassComputeStageExtents>();
    // and changes their dependencies
    optimizer.checkAndPushBack<PassSetSyncStage>();
    // validation check
    optimizer.checkAndPushBack<PassValidation>();
  }
  //===-----------------------------------------------------------------------------------------
  // // should be irrelevant now
  // optimizer.checkAndPushBack<PassStencilSplitter>(maxFields);
  // // but would require a lot
  //===-----------------------------------------------------------------------------------------
  if(shouldRunPass(options_, options_.MergeTemporaries)) {
    optimizer.checkAndPushBack<PassTemporaryMerger>();
    // this should not affect the temporaries but since we're touching them it would probably be a
    // safe idea
    optimizer.checkAndPushBack<PassTemporaryType>();
    optimizer.checkAndPushBack<PassLocalVarType>();
    // validation check
    optimizer.checkAndPushBack<PassValidation>();
  }
  //===-----------------------------------------------------------------------------------------
  if(shouldRunPass(options_, options_.Inlining)) {
    optimizer.checkAndPushBack<PassInlining>(
        (getOptions().Inlining || getOptions().PassTmpToFunction),
        PassInlining::InlineStrategy::ComputationsOnTheFly);
    // validation check
    optimizer.checkAndPushBack<PassValidation>();
  }
  //===-----------------------------------------------------------------------------------------
  if(shouldRunPass(options_, options_.PartitionIntervals)) {
    if(options_.PartitionIntervals) {
      optimizer.checkAndPushBack<PassIntervalPartitioning>();
      // since this can change the scope of temporaries ...
      optimizer.checkAndPushBack<PassTemporaryType>();
      optimizer.checkAndPushBack<PassLocalVarType>();
      // optimizer.checkAndPushBack<PassFixVersionedInputFields>();
      // validation check
      optimizer.checkAndPushBack<PassValidation>();
    }
  }
  //===-----------------------------------------------------------------------------------------
  if(shouldRunPass(options_, options_.PassTmpToFunction)) {
    optimizer.checkAndPushBack<PassTemporaryToStencilFunction>();
    // validation check
    optimizer.checkAndPushBack<PassValidation>();
  }
  //===-----------------------------------------------------------------------------------------
  if(shouldRunPass(options_, options_.SetNonTempCaches)) {
    optimizer.checkAndPushBack<PassSetNonTempCaches>();
    // this should not affect the temporaries but since we're touching them it would probably be a
    // safe idea
    optimizer.checkAndPushBack<PassTemporaryType>();
    optimizer.checkAndPushBack<PassLocalVarType>();
    // validation check
    optimizer.checkAndPushBack<PassValidation>();
  }
  //===-----------------------------------------------------------------------------------------
  if(shouldRunPass(options_, options_.SetCaches)) {
    optimizer.checkAndPushBack<PassSetCaches>();
    // validation check
    optimizer.checkAndPushBack<PassValidation>();
  }
  //===-----------------------------------------------------------------------------------------
  if(shouldRunPass(options_, options_.SetBlockSize)) {
    optimizer.checkAndPushBack<PassSetBlockSize>();
    // validation check
    optimizer.checkAndPushBack<PassValidation>();
  }
  //===-----------------------------------------------------------------------------------------
  if(shouldRunPass(options_, options_.DataLocalityMetric)) {
    if(options_.DataLocalityMetric) {
      // Plain diagnostics, should not even be a pass but is independent
      optimizer.checkAndPushBack<PassDataLocalityMetric>();
      // validation check
      optimizer.checkAndPushBack<PassValidation>();
    }
  }
  if(options_.Backend == "cuda" || options_.SerializeIIR) {
    optimizer.checkAndPushBack<PassInlining>(true,
                                             PassInlining::InlineStrategy::ComputationsOnTheFly);
    // validation check
    optimizer.checkAndPushBack<PassValidation>();
  }
  //===-----------------------------------------------------------------------------------------

  DAWN_LOG(INFO) << "All the passes ran with the current command line arguments:";
  for(const auto& a : optimizer.getPassManager().getPasses()) {
    DAWN_LOG(INFO) << a->getName();
  }

  int i = 0;
  for(auto& stencil : optimizer.getStencilInstantiationMap()) {
    // Run optimization passes
    auto& instantiation = stencil.second;

    DAWN_LOG(INFO) << "Starting optimization and analysis passes for `" << instantiation->getName()
                   << "` ...";
    if(!optimizer.getPassManager().runAllPassesOnStencilInstantiation(optimizer, instantiation))
      throw std::runtime_error("An error occurred.");

    DAWN_LOG(INFO) << "Done with optimization and analysis passes for `" << instantiation->getName()
                   << "`";

    if(options_.SerializeIIR) {
      const auto p = std::filesystem::path(options_.OutputFile.empty()
                                               ? instantiation->getMetaData().getFileName()
                                               : options_.OutputFile);
      IIRSerializer::serialize(static_cast<std::string>(p.stem()) + "." + std::to_string(i) +
                                   ".iir",
                               instantiation, serializationKind);
      i++;
    }
    if(options_.DumpStencilInstantiation) {
      instantiation->dump();
    }
  }

  return optimizer.getStencilInstantiationMap();
}

std::unique_ptr<codegen::TranslationUnit>
DawnCompiler::generate(const std::map<std::string, std::shared_ptr<iir::StencilInstantiation>>&
                           stencilInstantiationMap) {
  // Generate code
  try {
    BackendType backend = parseBackendString(options_.Backend);
    switch(backend) {
    case BackendType::GridTools: {
      codegen::gt::GTCodeGen CG(stencilInstantiationMap, diagnostics_, options_.UseParallelEP,
                                options_.MaxHaloPoints);
      return CG.generateCode();
    }
    case BackendType::CXXNaive: {
      codegen::cxxnaive::CXXNaiveCodeGen CG(stencilInstantiationMap, diagnostics_,
                                            options_.MaxHaloPoints);
      return CG.generateCode();
    }
    case BackendType::CUDA: {
      const Array3i domain_size{options_.domain_size_i, options_.domain_size_j,
                                options_.domain_size_k};
      codegen::cuda::CudaCodeGen CG(stencilInstantiationMap, diagnostics_, options_.MaxHaloPoints,
                                    options_.nsms, options_.maxBlocksPerSM, domain_size);
      return CG.generateCode();
    }
    case BackendType::CXXNaiveIco: {
      codegen::cxxnaiveico::CXXNaiveIcoCodeGen CG(stencilInstantiationMap, diagnostics_,
                                                  options_.MaxHaloPoints);

      return CG.generateCode();
    }
    case BackendType::CXXOpt:
      dawn_unreachable("GTClangOptCXX not supported yet");
    }
  } catch(...) {
    diagnostics_.report(buildDiag("-backend", options_.Backend,
                                  "backend options must be : " +
                                      dawn::RangeToString(", ", "", "")(std::vector<std::string>{
                                          "gridtools", "c++-naive", "c++-opt", "c++-naive-ico"})));
    return nullptr;
  }
  return nullptr;
}

std::unique_ptr<codegen::TranslationUnit>
DawnCompiler::compile(const std::shared_ptr<SIR>& stencilIR) {
  diagnostics_.clear();
  diagnostics_.setFilename(stencilIR->Filename);

  // Parallelize the SIR
  std::map<std::string, std::shared_ptr<iir::StencilInstantiation>> stencilInstantiation;
  try {
    stencilInstantiation = lowerToIIR(stencilIR);
  } catch(...) {
    DAWN_LOG(INFO) << "Errors occurred. Skipping optimisation and code generation.";
    return nullptr;
  }

  if(diagnostics_.hasErrors()) {
    DAWN_LOG(INFO) << "Errors occurred. Skipping optimisation and code generation.";
    return nullptr;
  }

  // Optimize the IIR
  auto optimizedStencilInstantiation = optimize(stencilInstantiation);

  if(diagnostics_.hasErrors()) {
    DAWN_LOG(INFO) << "Errors occurred. Skipping code generation.";
    return nullptr;
  }
  // Generate the Code
  return generate(optimizedStencilInstantiation);
}

const DiagnosticsEngine& DawnCompiler::getDiagnostics() const { return diagnostics_; }
DiagnosticsEngine& DawnCompiler::getDiagnostics() { return diagnostics_; }

const Options& DawnCompiler::getOptions() const { return options_; }
Options& DawnCompiler::getOptions() { return options_; }

} // namespace dawn

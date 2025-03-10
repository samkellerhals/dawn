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

#include "dawn/CodeGen/Cuda-ico/CudaIcoCodeGen.h"

#include "ASTStencilBody.h"
#include "dawn/AST/ASTExpr.h"
#include "dawn/AST/ASTVisitor.h"
#include "dawn/AST/IterationSpace.h"
#include "dawn/AST/LocationType.h"
#include "dawn/CodeGen/CXXUtil.h"
#include "dawn/CodeGen/Cuda-ico/LocToStringUtils.h"
#include "dawn/CodeGen/Cuda/CodeGeneratorHelper.h"
#include "dawn/CodeGen/F90Util.h"
#include "dawn/CodeGen/IcoChainSizes.h"
#include "dawn/IIR/Field.h"
#include "dawn/IIR/Interval.h"
#include "dawn/IIR/LoopOrder.h"
#include "dawn/IIR/MultiStage.h"
#include "dawn/IIR/Stage.h"
#include "dawn/IIR/Stencil.h"
#include "dawn/SIR/SIR.h"
#include "dawn/Support/Assert.h"
#include "dawn/Support/Exception.h"
#include "dawn/Support/FileSystem.h"
#include "dawn/Support/Logger.h"
#include "dawn/Support/STLExtras.h"
#include "driver-includes/unstructured_interface.hpp"

#include <algorithm>
#include <csignal>
#include <memory>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

static bool intervalsConsistent(const dawn::iir::Stage& stage) {
  // get the intervals for this stage
  std::unordered_set<dawn::iir::Interval> intervals;
  for(const auto& doMethodPtr : dawn::iterateIIROver<dawn::iir::DoMethod>(stage)) {
    intervals.insert(doMethodPtr->getInterval());
  }

  bool consistentLo =
      std::all_of(intervals.begin(), intervals.end(), [&](const dawn::iir::Interval& interval) {
        return intervals.begin()->lowerLevel() == interval.lowerLevel();
      });

  bool consistentHi =
      std::all_of(intervals.begin(), intervals.end(), [&](const dawn::iir::Interval& interval) {
        return intervals.begin()->upperLevel() == interval.upperLevel();
      });

  return consistentHi && consistentLo;
}

namespace {
std::vector<int> getUsedFields(const dawn::iir::Stencil& stencil,
                               std::unordered_set<dawn::iir::Field::IntendKind> intend = {
                                   dawn::iir::Field::IntendKind::Output,
                                   dawn::iir::Field::IntendKind::InputOutput,
                                   dawn::iir::Field::IntendKind::Input}) {
  const auto& APIFields = stencil.getMetadata().getAPIFields();
  const auto& stenFields = stencil.getOrderedFields();
  auto usedAPIFields =
      dawn::makeRange(APIFields, [&stenFields](int f) { return stenFields.count(f); });

  std::vector<int> res;
  for(auto fieldID : usedAPIFields) {
    auto field = stenFields.at(fieldID);
    if(intend.count(field.field.getIntend())) {
      res.push_back(fieldID);
    }
  }

  return res;
}
std::vector<std::string> getUsedFieldsNames(
    const dawn::iir::Stencil& stencil,
    std::unordered_set<dawn::iir::Field::IntendKind> intend = {
        dawn::iir::Field::IntendKind::Output, dawn::iir::Field::IntendKind::InputOutput,
        dawn::iir::Field::IntendKind::Input}) {
  auto usedFields = getUsedFields(stencil, intend);
  std::vector<std::string> fieldsVec;
  for(auto fieldID : usedFields) {
    fieldsVec.push_back(stencil.getMetadata().getFieldNameFromAccessID(fieldID));
  }
  return fieldsVec;
}

std::vector<std::string> getGlobalsNames(const dawn::ast::GlobalVariableMap& globalsMap) {
  std::vector<std::string> globalsNames;
  for(const auto& global : globalsMap) {
    globalsNames.push_back(global.first);
  }
  return globalsNames;
}

void addGlobalsArgs(const dawn::ast::GlobalVariableMap& globalsMap,
                    dawn::codegen::MemberFunction& fun) {
  for(const auto& global : globalsMap) {
    std::string Name = global.first;
    std::string Type = dawn::ast::Value::typeToString(global.second.getType());
    fun.addArg(Type + " " + Name);
  }
}

std::string explodeToStr(const std::vector<std::string>& vec, const std::string sep = ", ") {
  std::string ret = "";
  bool first = true;
  for(const auto& el : vec) {
    if(!first) {
      ret += sep;
    }
    ret += el;
    first = false;
  }

  return ret;
}

std::string explodeUsedFields(const dawn::iir::Stencil& stencil,
                              std::unordered_set<dawn::iir::Field::IntendKind> intend = {
                                  dawn::iir::Field::IntendKind::Output,
                                  dawn::iir::Field::IntendKind::InputOutput,
                                  dawn::iir::Field::IntendKind::Input}) {
  return explodeToStr(getUsedFieldsNames(stencil, intend));
}

} // namespace

namespace dawn {
namespace codegen {
namespace cudaico {
std::unique_ptr<TranslationUnit>
run(const std::map<std::string, std::shared_ptr<iir::StencilInstantiation>>&
        stencilInstantiationMap,
    const Options& options) {
  CudaIcoCodeGen CG(
      stencilInstantiationMap, options.MaxHaloSize,
      options.OutputCHeader == "" ? std::nullopt : std::make_optional(options.OutputCHeader),
      options.OutputFortranInterface == "" ? std::nullopt
                                           : std::make_optional(options.OutputFortranInterface),
      options.AtlasCompatible, options.BlockSize, options.LevelsPerThread);

  return CG.generateCode();
}

CudaIcoCodeGen::CudaIcoCodeGen(const StencilInstantiationContext& ctx, int maxHaloPoints,
                               std::optional<std::string> outputCHeader,
                               std::optional<std::string> outputFortranInterface,
                               bool atlasCompatible, int blockSize, int levelsPerThread)
    : CodeGen(ctx, maxHaloPoints), codeGenOptions_{outputCHeader, outputFortranInterface,
                                                   atlasCompatible, blockSize, levelsPerThread} {}

CudaIcoCodeGen::~CudaIcoCodeGen() {}

class CollectIterationSpaces : public ast::ASTVisitorForwardingNonConst {

public:
  struct IterSpaceHash {
    std::size_t operator()(const ast::UnstructuredIterationSpace& space) const {
      std::size_t seed = 0;
      dawn::hash_combine(seed, space.Chain, space.IncludeCenter);
      return seed;
    }
  };

  void visit(const std::shared_ptr<ast::ReductionOverNeighborExpr>& expr) override {
    spaces_.insert(expr->getIterSpace());
    for(auto c : expr->getChildren()) {
      c->accept(*this);
    }
  }

  void visit(const std::shared_ptr<ast::LoopStmt>& stmt) override {
    auto chainDescr = dynamic_cast<const ast::ChainIterationDescr*>(stmt->getIterationDescrPtr());
    spaces_.insert(chainDescr->getIterSpace());
    for(auto c : stmt->getChildren()) {
      c->accept(*this);
    }
  }

  const std::unordered_set<ast::UnstructuredIterationSpace, IterSpaceHash>& getSpaces() const {
    return spaces_;
  }

private:
  std::unordered_set<ast::UnstructuredIterationSpace, IterSpaceHash> spaces_;
};

void CudaIcoCodeGen::generateGpuMesh(
    const std::shared_ptr<iir::StencilInstantiation>& stencilInstantiation,
    Class& stencilWrapperClass, CodeGenProperties& codeGenProperties) {
  Structure gpuMeshClass = stencilWrapperClass.addStruct("GpuTriMesh");

  gpuMeshClass.addMember("int", "NumVertices");
  gpuMeshClass.addMember("int", "NumEdges");
  gpuMeshClass.addMember("int", "NumCells");
  gpuMeshClass.addMember("int", "VertexStride");
  gpuMeshClass.addMember("int", "EdgeStride");
  gpuMeshClass.addMember("int", "CellStride");
  gpuMeshClass.addMember("dawn::unstructured_domain", "HorizontalDomain");

  CollectIterationSpaces spaceCollector;
  std::unordered_set<ast::UnstructuredIterationSpace, CollectIterationSpaces::IterSpaceHash> spaces;
  for(const auto& doMethod : iterateIIROver<iir::DoMethod>(*(stencilInstantiation->getIIR()))) {
    doMethod->getAST().accept(spaceCollector);
    spaces.insert(spaceCollector.getSpaces().begin(), spaceCollector.getSpaces().end());
  }
  for(auto space : spaces) {
    gpuMeshClass.addMember("int*", chainToTableString(space));
  }
  {
    auto gpuMeshDefaultCtor = gpuMeshClass.addConstructor();
    gpuMeshDefaultCtor.startBody();
    gpuMeshDefaultCtor.commit();
  }
  {
    auto gpuMeshFromGlobalCtor = gpuMeshClass.addConstructor();
    gpuMeshFromGlobalCtor.addArg("const dawn::GlobalGpuTriMesh *mesh");
    gpuMeshFromGlobalCtor.addStatement("NumVertices = mesh->NumVertices");
    gpuMeshFromGlobalCtor.addStatement("NumCells = mesh->NumCells");
    gpuMeshFromGlobalCtor.addStatement("NumEdges = mesh->NumEdges");
    gpuMeshFromGlobalCtor.addStatement("VertexStride = mesh->VertexStride");
    gpuMeshFromGlobalCtor.addStatement("CellStride = mesh->CellStride");
    gpuMeshFromGlobalCtor.addStatement("EdgeStride = mesh->EdgeStride");
    gpuMeshFromGlobalCtor.addStatement("HorizontalDomain = mesh->HorizontalDomain");
    for(auto space : spaces) {
      gpuMeshFromGlobalCtor.addStatement(chainToTableString(space) + " = mesh->NeighborTables.at(" +
                                         "std::tuple<std::vector<dawn::LocationType>, bool>{" +
                                         chainToVectorString(space) + ", " +
                                         std::to_string(space.IncludeCenter) + "})");
    }
  }
}

void CudaIcoCodeGen::generateGridFun(MemberFunction& gridFun) {
  gridFun.addBlockStatement("if (kparallel)", [&]() {
    gridFun.addStatement("int dK = (kSize + LEVELS_PER_THREAD - 1) / LEVELS_PER_THREAD");
    gridFun.addStatement("return dim3((elSize + BLOCK_SIZE - 1) / BLOCK_SIZE, dK, 1)");
  });
  gridFun.addBlockStatement("else", [&]() {
    gridFun.addStatement("return dim3((elSize + BLOCK_SIZE - 1) / BLOCK_SIZE, 1, 1)");
  });
}

void CudaIcoCodeGen::generateRunFun(
    const std::shared_ptr<iir::StencilInstantiation>& stencilInstantiation, MemberFunction& runFun,
    CodeGenProperties& codeGenProperties) {

  const auto& globalsMap = stencilInstantiation->getIIR()->getGlobalVariableMap();

  runFun.addBlockStatement("if (!is_setup_)", [&]() {
    std::string stencilName = stencilInstantiation->getName();
    runFun.addStatement("printf(\"" + stencilName +
                        " has not been set up! make sure setup() is called before run!\\n\")");
    runFun.addStatement("return");
  });

  // find block sizes to generate
  std::set<ast::LocationType> stageLocType;
  for(const auto& ms : iterateIIROver<iir::MultiStage>(*(stencilInstantiation->getIIR()))) {
    for(const auto& stage : ms->getChildren()) {
      stageLocType.insert(*stage->getLocationType());
    }
  }
  runFun.addStatement("dim3 dB(BLOCK_SIZE, 1, 1)");

  std::string stencilName = stencilInstantiation->getName();

  for(const auto& ms : iterateIIROver<iir::MultiStage>(*(stencilInstantiation->getIIR()))) {
    for(const auto& stage : ms->getChildren()) {

      // fields used in the stencil
      const auto fields = support::orderMap(stage->getFields());

      // lets figure out how many k levels we need to consider
      std::stringstream k_size;
      DAWN_ASSERT_MSG(intervalsConsistent(*stage),
                      "intervals in a stage must have same Levels for now!\n");
      auto interval = stage->getChild(0)->getInterval();
      if(interval.levelIsEnd(iir::Interval::Bound::upper) &&
         interval.levelIsEnd(iir::Interval::Bound::lower)) {
        k_size << (interval.upperOffset() - interval.lowerOffset());
      } else if(interval.levelIsEnd(iir::Interval::Bound::upper)) {
        k_size << "kSize_ + " << interval.upperOffset() << " - "
               << (interval.lowerOffset() + interval.lowerLevel());
      } else {
        k_size << interval.upperLevel() << " + " << interval.upperOffset() << " - "
               << (interval.lowerOffset() + interval.lowerLevel());
      }

      // lets build correct block size
      std::string numElements;

      auto spaceMagicNumToEnum = [](int magicNum) -> std::string {
        switch(magicNum) {
        case 0:
          return "dawn::UnstructuredSubdomain::LateralBoundary";
        case 1000:
          return "dawn::UnstructuredSubdomain::Nudging";
        case 2000:
          return "dawn::UnstructuredSubdomain::Interior";
        case 3000:
          return "dawn::UnstructuredSubdomain::Halo";
        case 4000:
          return "dawn::UnstructuredSubdomain::End";
        default:
          throw std::runtime_error("Invalid magic number");
        }
      };

      auto numElementsString = [&](ast::LocationType loc,
                                   std::optional<iir::Interval> iterSpace) -> std::string {
        return iterSpace.has_value()
                   ? "mesh_.HorizontalDomain({::dawn::LocationType::" + locToStringPlural(loc) +
                         "," + spaceMagicNumToEnum(iterSpace->upperLevel()) + "," +
                         std::to_string(iterSpace->upperOffset()) + "})" +
                         "- mesh_.HorizontalDomain({::dawn::LocationType::" +
                         locToStringPlural(loc) + "," +
                         spaceMagicNumToEnum(iterSpace->lowerLevel()) + "," +
                         std::to_string(iterSpace->lowerOffset()) + "})"
                   : "mesh_.Num" + locToStringPlural(loc);
      };
      auto hOffsetSizeString = [&](ast::LocationType loc, iir::Interval iterSpace) -> std::string {
        return "mesh_.HorizontalDomain({::dawn::LocationType::" + locToStringPlural(loc) + "," +
               spaceMagicNumToEnum(iterSpace.lowerLevel()) + "," +
               std::to_string(iterSpace.lowerOffset()) + "})";
      };

      std::string iskparallel =
          (ms->getLoopOrder() == iir::LoopOrderKind::Parallel) ? "true" : "false";

      auto domain = stage->getUnstructuredIterationSpace();
      std::string hSizeString = "hsize" + std::to_string(stage->getStageID());
      std::string numElString = "mesh_." + locToStrideString(*stage->getLocationType());
      std::string hOffsetString = "hoffset" + std::to_string(stage->getStageID());
      if(domain.has_value()) {
        runFun.addStatement("int " + hOffsetString + " = " +
                            hOffsetSizeString(*stage->getLocationType(), *domain));
      } else {
        runFun.addStatement("int " + hOffsetString + " = 0");
      }
      runFun.addStatement("int " + hSizeString + " = " +
                          numElementsString(*stage->getLocationType(), domain));
      runFun.addBlockStatement("if (" + hSizeString + " != 0)", [&]() {
        runFun.addStatement("dim3 dG" + std::to_string(stage->getStageID()) + " = grid(" +
                            k_size.str() + ", " + hSizeString + "," + iskparallel + ")");

        //--------------------------------------
        // signature of kernel
        //--------------------------------------
        std::stringstream kernelCall;
        std::string kName =
            cuda::CodeGeneratorHelper::buildCudaKernelName(stencilInstantiation, ms, stage);
        kernelCall << kName;

        // which nbh tables need to be passed / which templates need to be defined?
        CollectIterationSpaces chainStringCollector;
        for(const auto& doMethod : stage->getChildren()) {
          doMethod->getAST().accept(chainStringCollector);
        }
        auto chains = chainStringCollector.getSpaces();

        if(chains.size() != 0) {
          kernelCall << "<";
          bool first = true;
          for(auto chain : chains) {
            if(!first) {
              kernelCall << ", ";
            }
            kernelCall << chainToSparseSizeString(chain);
            first = false;
          }
          kernelCall << ">";
        }

        kernelCall << "<<<"
                   << "dG" + std::to_string(stage->getStageID()) + ",dB,"
                   << "0, stream_"
                   << ">>>(";
        if(!globalsMap.empty()) {
          kernelCall << "m_globals, ";
        }
        kernelCall << numElString << ", ";

        // which loc size args (int CellIdx, int EdgeIdx, int CellIdx) need to be passed
        // additionally?
        std::set<std::string> locArgs;
        for(auto field : fields) {
          if(field.second.getFieldDimensions().isVertical()) {
            continue;
          }
          auto dims = ast::dimension_cast<ast::UnstructuredFieldDimension const&>(
              field.second.getFieldDimensions().getHorizontalFieldDimension());
          // dont add sizes twice
          if(dims.getDenseLocationType() == *stage->getLocationType()) {
            continue;
          }
          locArgs.insert(locToStrideString(dims.getDenseLocationType()));
        }
        for(auto arg : locArgs) {
          kernelCall << "mesh_." + arg + ", ";
        }

        // we always need the k size
        kernelCall << "kSize_, ";

        // in case of horizontal iteration space we need the offset
        if(domain.has_value()) {
          kernelCall << hOffsetString << ", ";
        }
        kernelCall << hSizeString << ", ";

        for(auto chain : chains) {
          kernelCall << "mesh_." + chainToTableString(chain) + ", ";
        }

        // field arguments (correct cv specifier)
        bool first = true;
        for(const auto& fieldPair : fields) {
          if(!first) {
            kernelCall << ", ";
          }
          kernelCall << stencilInstantiation->getMetaData().getFieldNameFromAccessID(
                            fieldPair.second.getAccessID()) +
                            "_";
          first = false;
        }
        kernelCall << ")";
        runFun.addStatement(kernelCall.str());
      });
      runFun.addPreprocessorDirective("ifndef NDEBUG\n");
      runFun.addStatement("gpuErrchk(cudaPeekAtLastError())");
      runFun.addStatement("gpuErrchk(cudaDeviceSynchronize())");
      runFun.addPreprocessorDirective("endif\n");
    }
  }
}

static void allocTempFields(MemberFunction& ctor, const iir::Stencil& stencil) {
  if(stencil.getMetadata()
         .hasAccessesOfType<iir::FieldAccessType::InterStencilTemporary,
                            iir::FieldAccessType::StencilTemporary>()) {
    for(auto accessID : stencil.getMetadata()
                            .getAccessesOfType<iir::FieldAccessType::InterStencilTemporary,
                                               iir::FieldAccessType::StencilTemporary>()) {

      auto fname = stencil.getMetadata().getFieldNameFromAccessID(accessID);
      auto dims = stencil.getMetadata().getFieldDimensions(accessID);
      if(dims.isVertical()) {
        ctor.addStatement("::dawn::allocField(&" +
                          stencil.getMetadata().getNameFromAccessID(accessID) + "_, kSize_)");
        continue;
      }

      bool isHorizontal = !dims.K();
      std::string kSizeStr = (isHorizontal) ? "1" : "kSize_";

      auto hdims = ast::dimension_cast<ast::UnstructuredFieldDimension const&>(
          dims.getHorizontalFieldDimension());
      if(hdims.isDense()) {
        ctor.addStatement("::dawn::allocField(&" + fname + "_, mesh_." +
                          locToStrideString(hdims.getDenseLocationType()) + ", " + kSizeStr + ")");
      } else {
        ctor.addStatement("::dawn::allocField(&" + fname + "_, " + "mesh_." +
                          locToStrideString(hdims.getDenseLocationType()) + ", " +
                          chainToSparseSizeString(hdims.getIterSpace()) + ", " + kSizeStr + ")");
      }
    }
  }
}

void CudaIcoCodeGen::generateStencilFree(MemberFunction& stencilFree, const iir::Stencil& stencil) {
  stencilFree.startBody();
  for(auto accessID : stencil.getMetadata()
                          .getAccessesOfType<iir::FieldAccessType::InterStencilTemporary,
                                             iir::FieldAccessType::StencilTemporary>()) {
    auto fname = stencil.getMetadata().getFieldNameFromAccessID(accessID);
    stencilFree.addStatement("gpuErrchk(cudaFree(" + fname + "_))");
  }
}

void CudaIcoCodeGen::generateStencilSetup(MemberFunction& stencilSetup,
                                          const iir::Stencil& stencil) {
  stencilSetup.addStatement("mesh_ = GpuTriMesh(mesh)");
  stencilSetup.addStatement("kSize_ = kSize");
  stencilSetup.addStatement("is_setup_ = true");
  stencilSetup.addStatement("stream_ = stream");
  for(const auto& fieldName :
      getUsedFieldsNames(stencil, {dawn::iir::Field::IntendKind::Output,
                                   dawn::iir::Field::IntendKind::InputOutput})) {
    stencilSetup.addStatement(fieldName + "_kSize_ = " + fieldName + "_kSize");
  }
  allocTempFields(stencilSetup, stencil);
}

void CudaIcoCodeGen::generateCopyMemoryFun(MemberFunction& copyFun,
                                           const iir::Stencil& stencil) const {
  auto usedAPIFields = getUsedFields(stencil);

  for(auto fieldID : usedAPIFields) {
    auto fname = stencil.getMetadata().getFieldNameFromAccessID(fieldID);
    copyFun.addArg("::dawn::float_type* " + fname);
  }
  copyFun.addArg("bool do_reshape");

  // call initField on each field
  for(auto fieldID : usedAPIFields) {
    auto fname = stencil.getMetadata().getFieldNameFromAccessID(fieldID);
    auto dims = stencil.getMetadata().getFieldDimensions(fieldID);
    if(dims.isVertical()) {
      copyFun.addStatement("dawn::initField(" + fname + ", " + "&" + fname + "_, kSize_)");
      continue;
    }

    bool isHorizontal = !dims.K();
    std::string kSizeStr = (isHorizontal) ? "1" : "kSize_";

    auto hdims = ast::dimension_cast<ast::UnstructuredFieldDimension const&>(
        dims.getHorizontalFieldDimension());
    if(hdims.isDense()) {
      copyFun.addStatement("dawn::initField(" + fname + ", " + "&" + fname + "_, " + "mesh_." +
                           locToStrideString(hdims.getDenseLocationType()) + ", " + kSizeStr +
                           ", do_reshape)");
    } else {
      copyFun.addStatement("dawn::initSparseField(" + fname + ", " + "&" + fname + "_, " +
                           "mesh_." + locToStrideString(hdims.getNeighborChain()[0]) + ", " +
                           chainToSparseSizeString(hdims.getIterSpace()) + ", " + kSizeStr +
                           ", do_reshape)");
    }
  }
}

void CudaIcoCodeGen::generateCopyPtrFun(MemberFunction& copyFun,
                                        const iir::Stencil& stencil) const {
  auto usedAPIFields = getUsedFields(stencil);

  for(auto fieldID : usedAPIFields) {
    auto fname = stencil.getMetadata().getFieldNameFromAccessID(fieldID);
    copyFun.addArg("::dawn::float_type* " + fname);
  }

  // copy pointer to each field storage
  for(auto fieldID : usedAPIFields) {
    auto fname = stencil.getMetadata().getFieldNameFromAccessID(fieldID);
    copyFun.addStatement(fname + "_ = " + fname);
  }
}

void CudaIcoCodeGen::generateCopyBackFun(MemberFunction& copyBackFun, const iir::Stencil& stencil,
                                         bool rawPtrs) const {
  const auto& fieldInfos = stencil.getOrderedFields();
  auto usedAPIFields = getUsedFields(
      stencil, {dawn::iir::Field::IntendKind::Output, dawn::iir::Field::IntendKind::InputOutput});

  // signature
  for(auto fieldID : usedAPIFields) {
    const auto& field = fieldInfos.at(fieldID);

    if(field.field.getFieldDimensions().isVertical()) {
      if(rawPtrs) {
        copyBackFun.addArg("::dawn::float_type* " + field.Name);
      } else {
        copyBackFun.addArg("dawn::vertical_field_t<LibTag, ::dawn::float_type>& " + field.Name);
      }
      continue;
    }

    auto dims = ast::dimension_cast<ast::UnstructuredFieldDimension const&>(
        field.field.getFieldDimensions().getHorizontalFieldDimension());
    if(rawPtrs) {
      copyBackFun.addArg("::dawn::float_type* " + field.Name);
    } else {
      if(dims.isDense()) {
        copyBackFun.addArg(locToDenseTypeString(dims.getDenseLocationType()) + "& " + field.Name);
      } else {
        copyBackFun.addArg(locToSparseTypeString(dims.getDenseLocationType()) + "& " + field.Name);
      }
    }
  }

  copyBackFun.addArg("bool do_reshape");

  auto getNumElements = [&](const iir::Stencil::FieldInfo& field) -> std::string {
    if(rawPtrs) {
      if(field.field.getFieldDimensions().isVertical()) {
        return "kSize_";
      }

      auto hdims = ast::dimension_cast<ast::UnstructuredFieldDimension const&>(
          field.field.getFieldDimensions().getHorizontalFieldDimension());

      std::string sizestr = "(mesh_.";
      if(hdims.isDense()) {
        sizestr += locToStrideString(hdims.getDenseLocationType()) + ")";
      } else {
        sizestr += locToStrideString(hdims.getDenseLocationType()) + ")" + "*" +
                   chainToSparseSizeString(hdims.getIterSpace());
      }
      if(field.field.getFieldDimensions().K()) {
        sizestr += " * kSize_";
      }
      return sizestr;
    } else {
      return field.Name + ".numElements()";
    }
  };

  // function body
  for(auto fieldID : usedAPIFields) {
    const auto& field = fieldInfos.at(fieldID);

    copyBackFun.addBlockStatement("if (do_reshape)", [&]() {
      copyBackFun.addStatement("::dawn::float_type* host_buf = new ::dawn::float_type[" +
                               getNumElements(field) + "]");
      copyBackFun.addStatement("gpuErrchk(cudaMemcpy((::dawn::float_type*) host_buf, " +
                               field.Name + "_, " + getNumElements(field) +
                               "*sizeof(::dawn::float_type), cudaMemcpyDeviceToHost))");

      if(!field.field.getFieldDimensions().isVertical()) {
        auto dims = ast::dimension_cast<ast::UnstructuredFieldDimension const&>(
            field.field.getFieldDimensions().getHorizontalFieldDimension());

        bool isHorizontal = !field.field.getFieldDimensions().K();
        std::string kSizeStr = (isHorizontal) ? "1" : "kSize_";

        if(dims.isDense()) {
          copyBackFun.addStatement("dawn::reshape_back(host_buf, " + field.Name +
                                   ((!rawPtrs) ? ".data()" : "") + " , " + kSizeStr + ", mesh_." +
                                   locToStrideString(dims.getDenseLocationType()) + ")");
        } else {
          copyBackFun.addStatement("dawn::reshape_back(host_buf, " + field.Name +
                                   ((!rawPtrs) ? ".data()" : "") + ", " + kSizeStr + ", mesh_." +
                                   locToStrideString(dims.getDenseLocationType()) + ", " +
                                   chainToSparseSizeString(dims.getIterSpace()) + ")");
        }
      }
      copyBackFun.addStatement("delete[] host_buf");
    });
    copyBackFun.addBlockStatement("else", [&]() {
      copyBackFun.addStatement(
          "gpuErrchk(cudaMemcpy(" + field.Name + ((!rawPtrs) ? ".data()" : "") + ", " + field.Name +
          "_," + getNumElements(field) + "*sizeof(::dawn::float_type), cudaMemcpyDeviceToHost))");
    });
  }
}

void CudaIcoCodeGen::generateStencilClasses(
    const std::shared_ptr<iir::StencilInstantiation>& stencilInstantiation,
    Class& stencilWrapperClass, CodeGenProperties& codeGenProperties) {
  const auto& stencils = stencilInstantiation->getStencils();
  const auto& globalsMap = stencilInstantiation->getIIR()->getGlobalVariableMap();

  // Stencil members:
  // generate the code for each of the stencils
  for(std::size_t stencilIdx = 0; stencilIdx < stencils.size(); ++stencilIdx) {
    const auto& stencil = *stencils[stencilIdx];

    std::string stencilName =
        codeGenProperties.getStencilName(StencilContext::SC_Stencil, stencil.getStencilID());

    Structure stencilClass = stencilWrapperClass.addStruct(stencilName, "");

    generateGlobalsAPI(stencilClass, globalsMap, codeGenProperties);

    // generate members (fields + kSize + gpuMesh)
    stencilClass.changeAccessibility("private");
    auto temporaries = stencil.getMetadata()
                           .getAccessesOfType<iir::FieldAccessType::InterStencilTemporary,
                                              iir::FieldAccessType::StencilTemporary>();
    for(auto field : support::orderMap(stencil.getFields())) {
      if(temporaries.count(stencil.getMetadata().getAccessIDFromName(field.second.Name))) {
        stencilClass.addMember("static ::dawn::float_type*", field.second.Name + "_");
      } else {
        stencilClass.addMember("::dawn::float_type*", field.second.Name + "_");
      }
    }
    stencilClass.addMember("static int", "kSize_");
    stencilClass.addMember("static GpuTriMesh", "mesh_");
    stencilClass.addMember("static bool", "is_setup_");
    stencilClass.addMember("static cudaStream_t", "stream_");

    for(auto fieldID : getUsedFields(stencil, {dawn::iir::Field::IntendKind::Output,
                                               dawn::iir::Field::IntendKind::InputOutput})) {
      stencilClass.addMember("static int",
                             stencilInstantiation->getMetaData().getNameFromAccessID(fieldID) +
                                 "_kSize_");
    }

    stencilClass.changeAccessibility("public");
    auto meshGetter = stencilClass.addMemberFunction("static const GpuTriMesh &", "getMesh");
    meshGetter.finishArgs();
    meshGetter.startBody();
    meshGetter.addStatement("return mesh_");
    meshGetter.commit();

    auto streamGetter = stencilClass.addMemberFunction("static cudaStream_t", "getStream");
    streamGetter.finishArgs();
    streamGetter.startBody();
    streamGetter.addStatement("return stream_");
    streamGetter.commit();

    auto kSizeGetter = stencilClass.addMemberFunction("static int", "getKSize");
    kSizeGetter.finishArgs();
    kSizeGetter.startBody();
    kSizeGetter.addStatement("return kSize_");
    kSizeGetter.commit();

    for(auto fieldID : getUsedFields(stencil, {dawn::iir::Field::IntendKind::Output,
                                               dawn::iir::Field::IntendKind::InputOutput})) {
      auto kSizeFieldGetter = stencilClass.addMemberFunction(
          "static int",
          "get_" + stencilInstantiation->getMetaData().getNameFromAccessID(fieldID) + "_KSize");
      kSizeFieldGetter.finishArgs();
      kSizeFieldGetter.startBody();
      kSizeFieldGetter.addStatement(
          "return " + stencilInstantiation->getMetaData().getNameFromAccessID(fieldID) + "_kSize_");
      kSizeFieldGetter.commit();
    }

    if(!globalsMap.empty()) {
      stencilClass.addMember("globals", "m_globals");
    }

    // constructor from library
    auto stencilClassFree = stencilClass.addMemberFunction("static void", "free");
    generateStencilFree(stencilClassFree, stencil);
    stencilClassFree.commit();

    auto stencilClassSetup = stencilClass.addMemberFunction("static void", "setup");
    stencilClassSetup.addArg("const dawn::GlobalGpuTriMesh *mesh");
    stencilClassSetup.addArg("int kSize");
    stencilClassSetup.addArg("cudaStream_t stream");
    for(auto fieldID : getUsedFields(stencil, {dawn::iir::Field::IntendKind::Output,
                                               dawn::iir::Field::IntendKind::InputOutput})) {
      stencilClassSetup.addArg("const int " +
                               stencilInstantiation->getMetaData().getNameFromAccessID(fieldID) +
                               "_kSize");
    }
    generateStencilSetup(stencilClassSetup, stencil);
    stencilClassSetup.commit();

    // grid helper fun
    //    can not be placed in cuda utils sinze it needs LEVELS_PER_THREAD and BLOCK_SIZE, which
    //    are supposed to become compiler flags
    auto gridFun = stencilClass.addMemberFunction("dim3", "grid");
    gridFun.addArg("int kSize");
    gridFun.addArg("int elSize");
    gridFun.addArg("bool kparallel");

    generateGridFun(gridFun);

    gridFun.commit();

    // minmal ctor
    auto stencilClassDefaultConstructor = stencilClass.addConstructor();
    stencilClassDefaultConstructor.startBody();
    stencilClassDefaultConstructor.commit();

    // run method
    auto runFun = stencilClass.addMemberFunction("void", "run");
    generateRunFun(stencilInstantiation, runFun, codeGenProperties);
    runFun.commit();

    // copy back fun
    auto copyBackFunRawPtr = stencilClass.addMemberFunction("void", "CopyResultToHost");
    generateCopyBackFun(copyBackFunRawPtr, stencil, true);
    copyBackFunRawPtr.commit();

    // copy to funs
    auto copyMemoryFun = stencilClass.addMemberFunction("void", "copy_memory");
    generateCopyMemoryFun(copyMemoryFun, stencil);
    copyMemoryFun.commit();

    // copy to funs
    auto copyPtrFun = stencilClass.addMemberFunction("void", "copy_pointers");
    generateCopyPtrFun(copyPtrFun, stencil);
    copyPtrFun.commit();
  }
}

void CudaIcoCodeGen::generateAllAPIRunFunctions(
    std::stringstream& ssSW, const std::shared_ptr<iir::StencilInstantiation>& stencilInstantiation,
    CodeGenProperties& codeGenProperties, bool fromHost, bool onlyDecl) const {
  const auto& stencils = stencilInstantiation->getStencils();
  DAWN_ASSERT_MSG(stencils.size() <= 1, "code generation only for at most one stencil!\n");

  CollectIterationSpaces chainCollector;
  std::set<std::vector<ast::LocationType>> chains;
  for(const auto& doMethod : iterateIIROver<iir::DoMethod>(*(stencilInstantiation->getIIR()))) {
    doMethod->getAST().accept(chainCollector);
    chains.insert(chainCollector.getSpaces().begin(), chainCollector.getSpaces().end());
  }

  const std::string wrapperName = stencilInstantiation->getName();

  // two functions if from host (from c / from fort), one function if simply passing the pointers
  std::vector<std::stringstream> apiRunFunStreams(fromHost ? 2 : 1);
  { // stringstreams need to outlive the correspondind MemberFunctions
    std::vector<std::unique_ptr<MemberFunction>> apiRunFuns;
    if(fromHost) {
      apiRunFuns.push_back(
          std::make_unique<MemberFunction>("void", "run_" + wrapperName + "_from_c_host",
                                           apiRunFunStreams[0], /*indent level*/ 0, onlyDecl));
      apiRunFuns.push_back(
          std::make_unique<MemberFunction>("void", "run_" + wrapperName + "_from_fort_host",
                                           apiRunFunStreams[1], /*indent level*/ 0, onlyDecl));
    } else {
      apiRunFuns.push_back(std::make_unique<MemberFunction>(
          "void", "run_" + wrapperName, apiRunFunStreams[0], /*indent level*/ 0, onlyDecl));
    }

    const auto& globalsMap = stencilInstantiation->getIIR()->getGlobalVariableMap();

    if(fromHost) {
      for(auto& apiRunFun : apiRunFuns) {
        apiRunFun->addArg("dawn::GlobalGpuTriMesh *mesh");
        apiRunFun->addArg("int k_size");
        addGlobalsArgs(globalsMap, *apiRunFun);
      }
    } else {
      addGlobalsArgs(globalsMap, *apiRunFuns[0]);
    }
    for(auto& apiRunFun : apiRunFuns) {
      for(auto accessID : stencilInstantiation->getMetaData().getAPIFields()) {
        apiRunFun->addArg("::dawn::float_type *" +
                          stencilInstantiation->getMetaData().getNameFromAccessID(accessID));
      }
    }
    for(auto& apiRunFun : apiRunFuns) {
      apiRunFun->finishArgs();
    }

    // Write body only when run for implementation generation
    if(!onlyDecl) {
      if(stencils.empty()) {
        for(auto& apiRunFun : apiRunFuns) {
          apiRunFun->startBody();
          apiRunFun->addStatement("return");
          apiRunFun->commit();
        }
      } else {
        // we now know that there is exactly one stencil
        const auto& stencil = *stencils[0];

        // listing all API fields
        std::string fieldsStr = explodeUsedFields(stencil);

        // listing all input & output fields
        std::string ioFieldStr =
            explodeUsedFields(stencil, {dawn::iir::Field::IntendKind::Output,
                                        dawn::iir::Field::IntendKind::InputOutput});

        const std::string stencilName =
            codeGenProperties.getStencilName(StencilContext::SC_Stencil, stencil.getStencilID());
        const std::string fullStencilName =
            "dawn_generated::cuda_ico::" + wrapperName + "::" + stencilName;

        auto copyGlobals = [](const dawn::ast::GlobalVariableMap& globalsMap, MemberFunction& fun) {
          for(const auto& global : globalsMap) {
            std::string Name = global.first;
            std::string Type = dawn::ast::Value::typeToString(global.second.getType());
            fun.addStatement("s.set_" + Name + "(" + Name + ")");
          }
        };

        for(auto& apiRunFun : apiRunFuns) {
          apiRunFun->addStatement(fullStencilName + " s");
        }
        if(fromHost) {
          for(auto& apiRunFun : apiRunFuns) {
            std::string k_size_concat_string;
            for(const auto& fieldName :
                getUsedFieldsNames(stencil, {dawn::iir::Field::IntendKind::Output,
                                             dawn::iir::Field::IntendKind::InputOutput})) {
              k_size_concat_string += ", k_size";
            }
            apiRunFun->addStatement(fullStencilName + "::setup(mesh, k_size, 0" +
                                    k_size_concat_string + ")");
          }
          // depending if we are calling from c or from fortran, we need to transpose the data or
          // not
          apiRunFuns[0]->addStatement("s.copy_memory(" + fieldsStr + ", true)");
          apiRunFuns[1]->addStatement("s.copy_memory(" + fieldsStr + ", false)");
          for(auto& apiRunFun : apiRunFuns) {
            copyGlobals(globalsMap, *apiRunFun);
          }
        } else {
          apiRunFuns[0]->addStatement("s.copy_pointers(" + fieldsStr + ")");
          copyGlobals(globalsMap, *apiRunFuns[0]);
        }
        for(auto& apiRunFun : apiRunFuns) {
          apiRunFun->addStatement("s.run()");
        }
        if(fromHost) {
          apiRunFuns[0]->addStatement("s.CopyResultToHost(" + ioFieldStr + ", true)");
          apiRunFuns[1]->addStatement("s.CopyResultToHost(" + ioFieldStr + ", false)");
          for(auto& apiRunFun : apiRunFuns) {
            apiRunFun->addStatement(fullStencilName + "::free()");
          }
        }
        for(auto& apiRunFun : apiRunFuns) {
          apiRunFun->addStatement("return");
          apiRunFun->commit();
        }
      }

      for(const auto& stream : apiRunFunStreams) {
        ssSW << stream.str();
      }

    } else {
      for(auto& apiRunFun : apiRunFuns) {
        apiRunFun->commit();
      }
      for(const auto& stream : apiRunFunStreams) {
        ssSW << stream.str();
      }
    }
  }
}

void CudaIcoCodeGen::generateAllAPIVerifyFunctions(
    std::stringstream& ssSW, const std::shared_ptr<iir::StencilInstantiation>& stencilInstantiation,
    CodeGenProperties& codeGenProperties, bool onlyDecl) const {
  const auto& stencils = stencilInstantiation->getStencils();
  DAWN_ASSERT_MSG(stencils.size() <= 1, "code generation only for at most one stencil!\n");
  const auto& stencil = *stencils[0];

  const std::string wrapperName = stencilInstantiation->getName();
  std::string stencilName =
      codeGenProperties.getStencilName(StencilContext::SC_Stencil, stencil.getStencilID());
  const std::string fullStencilName =
      "dawn_generated::cuda_ico::" + wrapperName + "::" + stencilName;

  auto getSerializeCall = [](dawn::ast::LocationType locType) -> std::string {
    using dawn::ast::LocationType;
    switch(locType) {
    case LocationType::Edges:
      return "serialize_dense_edges";
      break;
    case LocationType::Cells:
      return "serialize_dense_cells";
      break;
    case LocationType::Vertices:
      return "serialize_dense_verts";
      break;
    default:
      dawn_unreachable("invalid location type");
    }
  };

  std::stringstream verifySS, runAndVerifySS;

  { // stringstreams need to outlive the correspondind MemberFunctions
    const auto& globalsMap = stencilInstantiation->getIIR()->getGlobalVariableMap();

    MemberFunction verifyAPI("bool", "verify_" + wrapperName, verifySS, /*indent level*/ 0,
                             onlyDecl);
    MemberFunction runAndVerifyAPI("void", "run_and_verify_" + wrapperName, runAndVerifySS,
                                   /*indent level*/ 0, onlyDecl);

    for(auto fieldID : getUsedFields(stencil, {dawn::iir::Field::IntendKind::Output,
                                               dawn::iir::Field::IntendKind::InputOutput})) {
      verifyAPI.addArg("const ::dawn::float_type *" +
                       stencilInstantiation->getMetaData().getNameFromAccessID(fieldID) + "_dsl");
      verifyAPI.addArg("const ::dawn::float_type *" +
                       stencilInstantiation->getMetaData().getNameFromAccessID(fieldID));
    }
    for(auto fieldID : getUsedFields(stencil, {dawn::iir::Field::IntendKind::Output,
                                               dawn::iir::Field::IntendKind::InputOutput})) {
      verifyAPI.addArg("const double " +
                       stencilInstantiation->getMetaData().getNameFromAccessID(fieldID) +
                       "_rel_tol");
      verifyAPI.addArg("const double " +
                       stencilInstantiation->getMetaData().getNameFromAccessID(fieldID) +
                       "_abs_tol");
    }
    verifyAPI.addArg("const int iteration");
    verifyAPI.finishArgs();

    addGlobalsArgs(globalsMap, runAndVerifyAPI);
    for(auto accessID : stencilInstantiation->getMetaData().getAPIFields()) {
      runAndVerifyAPI.addArg("::dawn::float_type *" +
                             stencilInstantiation->getMetaData().getNameFromAccessID(accessID));
    }
    for(auto fieldID : getUsedFields(stencil, {dawn::iir::Field::IntendKind::InputOutput,
                                               dawn::iir::Field::IntendKind::Output})) {
      runAndVerifyAPI.addArg("::dawn::float_type *" +
                             stencilInstantiation->getMetaData().getNameFromAccessID(fieldID) +
                             "_before");
    }
    for(auto fieldID : getUsedFields(stencil, {dawn::iir::Field::IntendKind::InputOutput,
                                               dawn::iir::Field::IntendKind::Output})) {
      runAndVerifyAPI.addArg("const double " +
                             stencilInstantiation->getMetaData().getNameFromAccessID(fieldID) +
                             "_rel_tol");
      runAndVerifyAPI.addArg("const double " +
                             stencilInstantiation->getMetaData().getNameFromAccessID(fieldID) +
                             "_abs_tol");
    }
    runAndVerifyAPI.finishArgs();

    if(!onlyDecl) {
      const auto& fieldInfos = stencil.getOrderedFields();

      verifyAPI.startBody();
      verifyAPI.addStatement("using namespace std::chrono");
      verifyAPI.addStatement("const auto &mesh = " + fullStencilName + "::" + "getMesh()");
      verifyAPI.addStatement("cudaStream_t stream = " + fullStencilName + "::" + "getStream()");
      verifyAPI.addStatement("int kSize = " + fullStencilName + "::" + "getKSize()");
      verifyAPI.addStatement(
          "high_resolution_clock::time_point t_start = high_resolution_clock::now()");
      verifyAPI.addStatement("bool isValid");

      for(auto fieldID : getUsedFields(stencil, {dawn::iir::Field::IntendKind::Output,
                                                 dawn::iir::Field::IntendKind::InputOutput})) {
        const auto& fieldInfo = fieldInfos.at(fieldID);

        DAWN_ASSERT_MSG(!fieldInfo.field.getFieldDimensions().isVertical(),
                        "vertical fields can not be output fields");

        auto unstrDims = ast::dimension_cast<const ast::UnstructuredFieldDimension&>(
            fieldInfo.field.getFieldDimensions().getHorizontalFieldDimension());

        verifyAPI.addStatement("int " + fieldInfo.Name + "_kSize = " + fullStencilName +
                               "::" + "get_" + fieldInfo.Name + "_KSize()");
        std::string num_lev =
            (!fieldInfo.field.getFieldDimensions().K()) ? "1" : fieldInfo.Name + "_kSize";
        std::string dense_stride =
            "(mesh." + locToStrideString(unstrDims.getDenseLocationType()) + ")";
        std::string indexOfLastHorElement =
            "(mesh." + locToDenseSizeStringGpuMesh(unstrDims.getDenseLocationType()) + " -1)";
        std::string num_el = dense_stride + " * " + num_lev;
        if(unstrDims.isSparse()) {
          num_el += " * dawn_generated::cuda_ico::" + wrapperName +
                    "::" + chainToSparseSizeString(unstrDims.getIterSpace());
        }

        verifyAPI.addStatement("isValid = ::dawn::verify_field(stream, " + num_el + ", " + fieldInfo.Name +
                               "_dsl" + "," + fieldInfo.Name + ", \"" + fieldInfo.Name + "\"" +
                               "," + fieldInfo.Name + "_rel_tol" + "," + fieldInfo.Name +
                               "_abs_tol" + ")");

        verifyAPI.addBlockStatement("if (!isValid)", [&]() {
          verifyAPI.addPreprocessorDirective("ifdef __SERIALIZE_ON_ERROR");
          if(!unstrDims.isSparse()) {
            // serialize actual field
            auto lt = unstrDims.getDenseLocationType();
            auto serializeCall = getSerializeCall(lt);
            verifyAPI.addStatement(serializeCall + "(0" + ", " + indexOfLastHorElement + ", " +
                                   num_lev + ", " + dense_stride + ", " + fieldInfo.Name + ", \"" +
                                   wrapperName + "\", \"" + fieldInfo.Name + "\", iteration)");
            // serialize dsl field
            verifyAPI.addStatement(serializeCall + "(0" + ", " + indexOfLastHorElement + ", " +
                                   num_lev + ", " + dense_stride + ", " + fieldInfo.Name + "_dsl" +
                                   ", \"" + wrapperName + "\", \"" + fieldInfo.Name + "_dsl" +
                                   "\", iteration)");
            verifyAPI.addStatement("std::cout << \"[DSL] serializing " + fieldInfo.Name +
                                   " as error is high.\\n\" << std::flush");
          } else {
            verifyAPI.addStatement("std::cout << \"[DSL] can not serialize sparse field " +
                                   fieldInfo.Name + ", error is high.\\n\" << std::flush");
          }
          verifyAPI.addPreprocessorDirective("endif");
        });
      }

      verifyAPI.addPreprocessorDirective("ifdef __SERIALIZE_ON_ERROR\n"); // newline requried
      verifyAPI.addStatement("serialize_flush_iter(\"" + wrapperName + "\", iteration)");
      verifyAPI.addPreprocessorDirective("endif");

      verifyAPI.addStatement(
          "high_resolution_clock::time_point t_end = high_resolution_clock::now()");
      verifyAPI.addStatement(
          "duration<double> timing = duration_cast<duration<double>>(t_end - t_start)");
      verifyAPI.addStatement("std::cout << \"[DSL] Verification took \" << timing.count() << \" "
                             "seconds.\\n\" << std::flush");
      verifyAPI.addStatement("return isValid");

      // TODO runAndVerifyAPI body
      runAndVerifyAPI.addStatement("static int iteration = 0");

      runAndVerifyAPI.addStatement("std::cout << \"[DSL] Running stencil " + wrapperName +
                                   " (\" << iteration << \") ...\\n\" << std::flush");

      auto getDSLFieldsNames = [&fieldInfos, &stencilInstantiation](
                                   const iir::Stencil& stencil) -> std::vector<std::string> {
        auto apiFields = stencilInstantiation->getMetaData().getAPIFields();

        std::vector<std::string> fieldNames;
        for(auto fieldID : apiFields) {

          if(fieldInfos.count(fieldID) == 0) {
            // field is unused, so we don't need a `..._before` variant
            fieldNames.push_back(stencilInstantiation->getMetaData().getNameFromAccessID(fieldID));
          } else {
            auto fieldInfo = fieldInfos.at(fieldID);
            if(fieldInfo.field.getIntend() == dawn::iir::Field::IntendKind::InputOutput ||
               fieldInfo.field.getIntend() == dawn::iir::Field::IntendKind::Output) {
              fieldNames.push_back(fieldInfo.Name + "_before");
            } else {
              fieldNames.push_back(fieldInfo.Name);
            }
          }
        }
        return fieldNames;
      };

      runAndVerifyAPI.addStatement("run_" + wrapperName + "(" +
                                   explodeToStr(concatenateVectors(
                                       {getGlobalsNames(globalsMap), getDSLFieldsNames(stencil)})) +
                                   ")");

      runAndVerifyAPI.addStatement("std::cout << \"[DSL] " + wrapperName +
                                   " run time: \" << "
                                   "time << \"s\\n\" << std::flush");
      runAndVerifyAPI.addStatement("std::cout << \"[DSL] Verifying stencil " + wrapperName +
                                   "...\\n\" << std::flush");

      std::vector<std::string> outputVerifyFields;
      for(const auto& fieldName :
          getUsedFieldsNames(stencil, {dawn::iir::Field::IntendKind::Output,
                                       dawn::iir::Field::IntendKind::InputOutput})) {
        outputVerifyFields.push_back(fieldName + "_before");
        outputVerifyFields.push_back(fieldName);
      }
      for(const auto& fieldName :
          getUsedFieldsNames(stencil, {dawn::iir::Field::IntendKind::Output,
                                       dawn::iir::Field::IntendKind::InputOutput})) {
        outputVerifyFields.push_back(fieldName + "_rel_tol");
        outputVerifyFields.push_back(fieldName + "_abs_tol");
      }

      runAndVerifyAPI.addStatement(
          "verify_" + wrapperName + "(" +
          explodeToStr(concatenateVectors({outputVerifyFields, {"iteration"}})) + ")");

      runAndVerifyAPI.addStatement("iteration++");
    }

    verifyAPI.commit();
    runAndVerifyAPI.commit();
    ssSW << verifySS.str();
    ssSW << runAndVerifySS.str();
  }
} // namespace cudaico

void CudaIcoCodeGen::generateMemMgmtFunctions(
    std::stringstream& ssSW, const std::shared_ptr<iir::StencilInstantiation>& stencilInstantiation,
    CodeGenProperties& codeGenProperties, bool onlyDecl) const {
  const std::string wrapperName = stencilInstantiation->getName();
  std::string stencilName = codeGenProperties.getStencilName(
      StencilContext::SC_Stencil, stencilInstantiation->getStencils()[0]->getStencilID());
  const std::string fullStencilName =
      "dawn_generated::cuda_ico::" + wrapperName + "::" + stencilName;
  const auto& stencils = stencilInstantiation->getStencils();
  DAWN_ASSERT_MSG(stencils.size() <= 1, "code generation only for at most one stencil!\n");
  const auto& stencil = *stencils[0];

  MemberFunction setupFun("void", "setup_" + wrapperName, ssSW, 0, onlyDecl);
  setupFun.addArg("dawn::GlobalGpuTriMesh *mesh");
  setupFun.addArg("int k_size");
  setupFun.addArg("cudaStream_t stream");
  for(const auto& fieldName :
      getUsedFieldsNames(stencil, {dawn::iir::Field::IntendKind::Output,
                                   dawn::iir::Field::IntendKind::InputOutput})) {
    setupFun.addArg("const int " + fieldName + "_k_size");
  }
  setupFun.finishArgs();
  if(!onlyDecl) {
    std::string k_size_concat_string;
    for(const auto& fieldName :
        getUsedFieldsNames(stencil, {dawn::iir::Field::IntendKind::Output,
                                     dawn::iir::Field::IntendKind::InputOutput})) {
      k_size_concat_string += ", " + fieldName + "_k_size";
    }
    setupFun.addStatement(fullStencilName + "::setup(mesh, k_size, stream" + k_size_concat_string +
                          ")");
  }
  setupFun.commit();

  MemberFunction freeFun("void", "free_" + wrapperName, ssSW, 0, onlyDecl);
  freeFun.finishArgs();
  if(!onlyDecl) {
    freeFun.startBody();
    freeFun.addStatement(fullStencilName + "::free()");
  }
  freeFun.commit();
}

void CudaIcoCodeGen::generateStaticMembersTrailer(
    std::stringstream& ssSW, const std::shared_ptr<iir::StencilInstantiation>& stencilInstantiation,
    CodeGenProperties& codeGenProperties) const {
  const auto& constStencils = stencilInstantiation->getStencils();
  DAWN_ASSERT_MSG(constStencils.size() <= 1, "code generation only for at most one stencil!\n");
  const auto& constStencil = *constStencils[0];
  auto& stencil = stencilInstantiation->getStencils()[0];
  const std::string wrapperName = stencilInstantiation->getName();
  std::string stencilName =
      codeGenProperties.getStencilName(StencilContext::SC_Stencil, stencil->getStencilID());
  const std::string fullStencilName =
      "dawn_generated::cuda_ico::" + wrapperName + "::" + stencilName;

  for(auto accessID : stencil->getMetadata()
                          .getAccessesOfType<iir::FieldAccessType::InterStencilTemporary,
                                             iir::FieldAccessType::StencilTemporary>()) {
    auto fname = stencil->getMetadata().getFieldNameFromAccessID(accessID);
    ssSW << "::dawn::float_type *" << fullStencilName << "::" << fname << "_;\n";
  }
  ssSW << "int " << fullStencilName << "::"
       << "kSize_;\n";
  ssSW << "cudaStream_t " << fullStencilName << "::"
       << "stream_;\n";
  for(const auto& fieldName :
      getUsedFieldsNames(constStencil, {dawn::iir::Field::IntendKind::Output,
                                        dawn::iir::Field::IntendKind::InputOutput})) {
    ssSW << "int " << fullStencilName << "::" << fieldName + "_kSize_;\n";
  }
  ssSW << "bool " << fullStencilName << "::"
       << "is_setup_ = false;\n";
  ssSW << "dawn_generated::cuda_ico::" + wrapperName << "::GpuTriMesh " << fullStencilName << "::"
       << "mesh_;\n";
}

std::string CudaIcoCodeGen::intervalBoundToString(const iir::Interval& interval,
                                                  iir::Interval::Bound bound) {
  if(interval.levelIsEnd(bound)) {
    return "kSize + " + std::to_string(interval.offset(bound));
  } else {
    return std::to_string(interval.offset(bound));
  }
}

void CudaIcoCodeGen::generateKIntervalBounds(MemberFunction& cudaKernel,
                                             const iir::Interval& interval,
                                             iir::LoopOrderKind loopOrder) {
  if(loopOrder == iir::LoopOrderKind::Parallel) {
    // if(false) {

    cudaKernel.addStatement("unsigned int kidx = blockIdx.y * blockDim.y + threadIdx.y");

    if(interval.lowerLevelIsEnd() && interval.upperLevelIsEnd()) {
      cudaKernel.addStatement("int klo = kidx * LEVELS_PER_THREAD + (kSize + " +
                              std::to_string(interval.lowerOffset()) + ")");
      cudaKernel.addStatement("int khi = (kidx + 1) * LEVELS_PER_THREAD + (kSize + " +
                              std::to_string(interval.lowerOffset()) + ")");

    } else {
      cudaKernel.addStatement("int klo = kidx * LEVELS_PER_THREAD + " +
                              std::to_string(interval.lowerOffset()));
      cudaKernel.addStatement("int khi = (kidx + 1) * LEVELS_PER_THREAD + " +
                              std::to_string(interval.lowerOffset()));
    }
  } else if(loopOrder == iir::LoopOrderKind::Forward) {
    cudaKernel.addStatement("int klo = " +
                            intervalBoundToString(interval, iir::Interval::Bound::lower));
    cudaKernel.addStatement("int khi = " +
                            intervalBoundToString(interval, iir::Interval::Bound::upper));
  } else {
    cudaKernel.addStatement(
        "int klo = " + intervalBoundToString(interval, iir::Interval::Bound::upper) + "-1");
    cudaKernel.addStatement(
        "int khi = " + intervalBoundToString(interval, iir::Interval::Bound::lower) + "-1");
  }
}

std::string CudaIcoCodeGen::incrementIterator(std::string iter, iir::LoopOrderKind loopOrder) {
  return iter + ((loopOrder == iir::LoopOrderKind::Backward) ? "--" : "++");
}

std::string CudaIcoCodeGen::comparisonOperator(iir::LoopOrderKind loopOrder) {
  return ((loopOrder == iir::LoopOrderKind::Backward) ? ">" : "<");
}

void CudaIcoCodeGen::generateAllCudaKernels(
    std::stringstream& ssSW,
    const std::shared_ptr<iir::StencilInstantiation>& stencilInstantiation) {
  ASTStencilBody stencilBodyCXXVisitor(stencilInstantiation->getMetaData(),
                                       codeGenOptions_.AtlasCompatible);
  const auto& globalsMap = stencilInstantiation->getIIR()->getGlobalVariableMap();

  for(const auto& ms : iterateIIROver<iir::MultiStage>(*(stencilInstantiation->getIIR()))) {
    for(const auto& stage : ms->getChildren()) {

      // fields used in the stencil
      const auto fields = support::orderMap(stage->getFields());

      //--------------------------------------
      // signature of kernel
      //--------------------------------------

      // which nbh tables / size templates need to be passed?
      CollectIterationSpaces chainStringCollector;
      for(const auto& doMethod : stage->getChildren()) {
        doMethod->getAST().accept(chainStringCollector);
      }
      auto chains = chainStringCollector.getSpaces();

      std::string retString = "__global__ void";
      if(chains.size() != 0) {
        std::stringstream ss;
        ss << "template<";
        bool first = true;
        for(auto chain : chains) {
          if(!first) {
            ss << ", ";
          }
          ss << "int " << chainToSparseSizeString(chain);
          first = false;
        }
        ss << ">";
        retString = ss.str() + retString;
      }
      MemberFunction cudaKernel(
          retString,
          cuda::CodeGeneratorHelper::buildCudaKernelName(stencilInstantiation, ms, stage), ssSW);

      if(!globalsMap.empty()) {
        cudaKernel.addArg("globals globals");
      }
      auto loc = *stage->getLocationType();
      cudaKernel.addArg("int " + locToStrideString(loc));

      // which additional loc size args ( int NumCells, int NumEdges, int NumVertices) need to
      // be passed?
      std::set<std::string> locArgs;
      for(auto field : fields) {
        if(field.second.getFieldDimensions().isVertical()) {
          continue;
        }
        auto dims = ast::dimension_cast<ast::UnstructuredFieldDimension const&>(
            field.second.getFieldDimensions().getHorizontalFieldDimension());
        if(dims.getDenseLocationType() == loc) {
          continue;
        }
        locArgs.insert(locToStrideString(dims.getDenseLocationType()));
      }
      for(auto arg : locArgs) {
        cudaKernel.addArg("int " + arg);
      }

      // we always need the k size
      cudaKernel.addArg("int kSize");

      // for horizontal iteration spaces we also need the offset
      if(stage->getUnstructuredIterationSpace().has_value()) {
        cudaKernel.addArg("int hOffset");
      }
      cudaKernel.addArg("int hSize");

      for(auto chain : chains) {
        cudaKernel.addArg("const int *" + chainToTableString(chain));
      }

      // field arguments (correct cv specifier)
      for(const auto& fieldPair : fields) {
        std::string cvstr = fieldPair.second.getIntend() == dawn::iir::Field::IntendKind::Input
                                ? "const ::dawn::float_type * __restrict__ "
                                : "::dawn::float_type * __restrict__ ";
        cudaKernel.addArg(cvstr + stencilInstantiation->getMetaData().getFieldNameFromAccessID(
                                      fieldPair.second.getAccessID()));
      }

      //--------------------------------------
      // body of the kernel
      //--------------------------------------

      DAWN_ASSERT_MSG(intervalsConsistent(*stage),
                      "intervals in a stage must have same Levels for now!\n");
      auto interval = stage->getChild(0)->getInterval();

      std::stringstream k_size;
      if(interval.levelIsEnd(iir::Interval::Bound::upper)) {
        k_size << "kSize + " << interval.upperOffset();
      } else {
        k_size << interval.upperLevel() << " + " << interval.upperOffset();
      }

      // pidx
      cudaKernel.addStatement("unsigned int pidx = blockIdx.x * blockDim.x + threadIdx.x");

      generateKIntervalBounds(cudaKernel, interval, ms->getLoopOrder());

      switch(loc) {
      case ast::LocationType::Cells:
        cudaKernel.addBlockStatement("if (pidx >= hSize)",
                                     [&]() { cudaKernel.addStatement("return"); });
        break;
      case ast::LocationType::Edges:
        cudaKernel.addBlockStatement("if (pidx >= hSize)",
                                     [&]() { cudaKernel.addStatement("return"); });
        break;
      case ast::LocationType::Vertices:
        cudaKernel.addBlockStatement("if (pidx >= hSize)",
                                     [&]() { cudaKernel.addStatement("return"); });
        break;
      }

      if(stage->getUnstructuredIterationSpace().has_value()) {
        cudaKernel.addStatement("pidx += hOffset");
      }

      // k loop (we ensured that all k intervals for all do methods in a stage are equal for
      // now)
      cudaKernel.addBlockStatement(
          "for(int kIter = klo; kIter " + comparisonOperator(ms->getLoopOrder()) + " khi; " +
              incrementIterator("kIter", ms->getLoopOrder()) + ")",
          [&]() {
            cudaKernel.addBlockStatement("if (kIter >= " + k_size.str() + ")",
                                         [&]() { cudaKernel.addStatement("return"); });
            for(const auto& doMethodPtr : stage->getChildren()) {
              // Generate Do-Method
              const iir::DoMethod& doMethod = *doMethodPtr;

              for(const auto& stmt : doMethod.getAST().getStatements()) {
                stmt->accept(stencilBodyCXXVisitor);
                cudaKernel << stencilBodyCXXVisitor.getCodeAndResetStream();
              }
            }
          });
    }
  }
}

std::string CudaIcoCodeGen::generateStencilInstantiation(
    const std::shared_ptr<iir::StencilInstantiation>& stencilInstantiation) {
  using namespace codegen;

  std::stringstream ssSW;

  Namespace dawnNamespace("dawn_generated", ssSW);
  Namespace cudaNamespace("cuda_ico", ssSW);

  generateAllCudaKernels(ssSW, stencilInstantiation);

  CollectIterationSpaces spaceCollector;
  std::unordered_set<ast::UnstructuredIterationSpace, CollectIterationSpaces::IterSpaceHash> spaces;
  for(const auto& doMethod : iterateIIROver<iir::DoMethod>(*(stencilInstantiation->getIIR()))) {
    doMethod->getAST().accept(spaceCollector);
    spaces.insert(spaceCollector.getSpaces().begin(), spaceCollector.getSpaces().end());
  }
  std::stringstream ss;
  bool first = true;
  for(auto space : spaces) {
    if(!first) {
      ss << ", ";
    }
    ss << "int " + chainToSparseSizeString(space) << " ";
    first = false;
  }
  Class stencilWrapperClass(stencilInstantiation->getName(), ssSW);
  stencilWrapperClass.changeAccessibility("public");
  for(auto space : spaces) {
    std::string spaceStr = std::to_string(ICOChainSize(space));
    if(space.IncludeCenter) {
      spaceStr += "+ 1";
    }
    stencilWrapperClass.addMember("static const int",
                                  chainToSparseSizeString(space) + " = " + spaceStr);
  }

  CodeGenProperties codeGenProperties = computeCodeGenProperties(stencilInstantiation.get());

  generateGpuMesh(stencilInstantiation, stencilWrapperClass, codeGenProperties);

  generateStencilClasses(stencilInstantiation, stencilWrapperClass, codeGenProperties);

  stencilWrapperClass.commit();

  cudaNamespace.commit();
  dawnNamespace.commit();
  ssSW << "extern \"C\" {\n";
  bool fromHost = true;
  generateAllAPIRunFunctions(ssSW, stencilInstantiation, codeGenProperties, fromHost);
  generateAllAPIRunFunctions(ssSW, stencilInstantiation, codeGenProperties, !fromHost);
  generateAllAPIVerifyFunctions(ssSW, stencilInstantiation, codeGenProperties);
  generateMemMgmtFunctions(ssSW, stencilInstantiation, codeGenProperties);
  ssSW << "}\n";
  generateStaticMembersTrailer(ssSW, stencilInstantiation, codeGenProperties);

  return ssSW.str();
}

void CudaIcoCodeGen::generateCHeaderSI(
    std::stringstream& ssSW,
    const std::shared_ptr<iir::StencilInstantiation>& stencilInstantiation) const {
  using namespace codegen;

  CodeGenProperties codeGenProperties = computeCodeGenProperties(stencilInstantiation.get());

  ssSW << "extern \"C\" {\n";
  bool fromHost = true;
  generateAllAPIRunFunctions(ssSW, stencilInstantiation, codeGenProperties, fromHost,
                             /*onlyDecl=*/true);
  generateAllAPIRunFunctions(ssSW, stencilInstantiation, codeGenProperties, !fromHost,
                             /*onlyDecl=*/true);
  generateAllAPIVerifyFunctions(ssSW, stencilInstantiation, codeGenProperties,
                                /*onlyDecl=*/true);
  generateMemMgmtFunctions(ssSW, stencilInstantiation, codeGenProperties, /*onlyDecl=*/true);
  ssSW << "}\n";
}

std::string CudaIcoCodeGen::generateCHeader() const {
  std::stringstream ssSW;
  ssSW << "#pragma once\n";
  ssSW << "#include \"driver-includes/defs.hpp\"\n";
  ssSW << "#include \"driver-includes/cuda_utils.hpp\"\n";

  for(const auto& nameStencilCtxPair : context_) {
    std::shared_ptr<iir::StencilInstantiation> stencilInstantiation = nameStencilCtxPair.second;
    generateCHeaderSI(ssSW, stencilInstantiation);
  }

  return ssSW.str();
}

static void
generateF90InterfaceSI(FortranInterfaceModuleGen& fimGen,
                       const std::shared_ptr<iir::StencilInstantiation>& stencilInstantiation) {
  const auto& stencils = stencilInstantiation->getStencils();
  const auto& globalsMap = stencilInstantiation->getIIR()->getGlobalVariableMap();
  auto globalTypeToFortType = [](const ast::Global& global) {
    switch(global.getType()) {
    case ast::Value::Kind::Boolean:
      return FortranAPI::InterfaceType::BOOLEAN;
    case ast::Value::Kind::Double:
      return FortranAPI::InterfaceType::DOUBLE;
    case ast::Value::Kind::Float:
      return FortranAPI::InterfaceType::FLOAT;
    case ast::Value::Kind::Integer:
      return FortranAPI::InterfaceType::INTEGER;
    case ast::Value::Kind::String:
    default:
      throw std::runtime_error("string globals not supported in cuda ico backend");
    }
  };

  // The following assert is needed because we have only one (user-defined) name for a stencil
  // instantiation (stencilInstantiation->getName()). We could compute a per-stencil name (
  // codeGenProperties.getStencilName(StencilContext::SC_Stencil, stencil.getStencilID()) )
  // however the interface would not be very useful if the name is generated.
  DAWN_ASSERT_MSG(stencils.size() <= 1,
                  "Unable to generate interface. More than one stencil in stencil instantiation.");
  const auto& stencil = *stencils[0];

  std::vector<FortranInterfaceAPI> interfaces = {
      FortranInterfaceAPI("run_" + stencilInstantiation->getName()),
      FortranInterfaceAPI("run_" + stencilInstantiation->getName() + "_from_fort_host"),
      FortranInterfaceAPI("run_and_verify_" + stencilInstantiation->getName())};

  FortranWrapperAPI runWrapper = FortranWrapperAPI("wrap_run_" + stencilInstantiation->getName());

  // only from host convenience wrapper takes mesh and k_size
  const int fromDeviceAPIIdx = 0;
  const int fromHostAPIIdx = 1;
  interfaces[fromHostAPIIdx].addArg("mesh", FortranAPI::InterfaceType::OBJ);
  interfaces[fromHostAPIIdx].addArg("k_size", FortranAPI::InterfaceType::INTEGER);

  const int runAndVerifyIdx = 2;

  auto addArgsToAPI = [&](FortranAPI& api, bool includeSavedState, bool optThresholds) {
    for(const auto& global : globalsMap) {
      api.addArg(global.first, globalTypeToFortType(global.second));
    }
    for(auto fieldID : stencilInstantiation->getMetaData().getAPIFields()) {
      api.addArg(
          stencilInstantiation->getMetaData().getNameFromAccessID(fieldID),
          FortranAPI::InterfaceType::DOUBLE /* Unfortunately we need to know at codegen
                                                        time whether we have fields in SP/DP */
          ,
          stencilInstantiation->getMetaData().getFieldDimensions(fieldID).rank());
    }
    if(includeSavedState) {
      for(auto fieldID : getUsedFields(stencil, {dawn::iir::Field::IntendKind::Output,
                                                 dawn::iir::Field::IntendKind::InputOutput})) {
        api.addArg(
            stencilInstantiation->getMetaData().getNameFromAccessID(fieldID) + "_before",
            FortranAPI::InterfaceType::DOUBLE /* Unfortunately we need to know at codegen
                                                          time whether we have fields in SP/DP */
            ,
            stencilInstantiation->getMetaData().getFieldDimensions(fieldID).rank());
      }

      for(auto fieldID : getUsedFields(stencil, {dawn::iir::Field::IntendKind::Output,
                                                 dawn::iir::Field::IntendKind::InputOutput})) {
        if(optThresholds) {
          api.addOptArg(stencilInstantiation->getMetaData().getNameFromAccessID(fieldID) +
                            "_rel_tol",
                        FortranAPI::InterfaceType::DOUBLE);
          api.addOptArg(stencilInstantiation->getMetaData().getNameFromAccessID(fieldID) +
                            "_abs_tol",
                        FortranAPI::InterfaceType::DOUBLE);
        } else {
          api.addArg(stencilInstantiation->getMetaData().getNameFromAccessID(fieldID) + "_rel_tol",
                     FortranAPI::InterfaceType::DOUBLE);
          api.addArg(stencilInstantiation->getMetaData().getNameFromAccessID(fieldID) + "_abs_tol",
                     FortranAPI::InterfaceType::DOUBLE);
        }
      }
    }
  };

  addArgsToAPI(interfaces[fromDeviceAPIIdx], /*includeSavedState*/ false, false);
  fimGen.addInterfaceAPI(std::move(interfaces[fromDeviceAPIIdx]));
  addArgsToAPI(interfaces[fromHostAPIIdx], /*includeSavedState*/ false, false);
  fimGen.addInterfaceAPI(std::move(interfaces[fromHostAPIIdx]));
  addArgsToAPI(interfaces[runAndVerifyIdx], /*includeSavedState*/ true, false);
  fimGen.addInterfaceAPI(std::move(interfaces[runAndVerifyIdx]));

  addArgsToAPI(runWrapper, /*includeSavedState*/ true, true);

  std::string fortranIndent = "   ";

  auto getFieldArgs = [&](bool includeSavedState) -> std::vector<std::string> {
    std::vector<std::string> args;
    for(auto fieldID : stencilInstantiation->getMetaData().getAPIFields()) {
      args.push_back(stencilInstantiation->getMetaData().getNameFromAccessID(fieldID));
    }
    if(includeSavedState) {
      for(auto fieldID : getUsedFields(stencil, {dawn::iir::Field::IntendKind::Output,
                                                 dawn::iir::Field::IntendKind::InputOutput})) {
        args.push_back(stencilInstantiation->getMetaData().getNameFromAccessID(fieldID) +
                       "_before");
      }
    }
    return args;
  };

  std::vector<std::string> threshold_names;
  for(auto fieldID : getUsedFields(stencil, {dawn::iir::Field::IntendKind::Output,
                                             dawn::iir::Field::IntendKind::InputOutput})) {
    threshold_names.push_back(stencilInstantiation->getMetaData().getNameFromAccessID(fieldID) +
                              "_rel_err_tol");
    threshold_names.push_back(stencilInstantiation->getMetaData().getNameFromAccessID(fieldID) +
                              "_abs_err_tol");
  }

  auto genCallArgs = [&](FortranWrapperAPI& wrapper, std::string first = "",
                         bool includeSavedState = false, bool includeErrorThreshold = false) {
    wrapper.addBodyLine("( &");

    if(first != "") {
      wrapper.addBodyLine(fortranIndent + first + ", &"); // TODO remove
    }

    auto args = concatenateVectors<std::string>(
        {getGlobalsNames(globalsMap), getFieldArgs(includeSavedState)});

    for(int i = 0; i < args.size(); ++i) {
      wrapper.addBodyLine(fortranIndent + args[i] +
                          ((i == (args.size() - 1)) && !includeErrorThreshold ? " &" : ", &"));
    }

    if(includeErrorThreshold) {
      for(int i = 0; i < threshold_names.size() - 1; i++) {
        wrapper.addBodyLine(fortranIndent + threshold_names[i] + ", &");
      }

      wrapper.addBodyLine(fortranIndent + threshold_names[threshold_names.size() - 1] + " &");
    }

    wrapper.addBodyLine(")");
  };

  runWrapper.addBodyLine("");

  for(int i = 0; i < threshold_names.size(); i++) {
    runWrapper.addBodyLine("real(c_double) :: " + threshold_names[i]);
  }

  runWrapper.addBodyLine("");
  for(auto fieldID : getUsedFields(stencil, {dawn::iir::Field::IntendKind::Output,
                                             dawn::iir::Field::IntendKind::InputOutput})) {
    runWrapper.addBodyLine("if (present(" +
                           stencilInstantiation->getMetaData().getNameFromAccessID(fieldID) +
                           "_rel_tol" + ")) then");
    runWrapper.addBodyLine(
        "  " + stencilInstantiation->getMetaData().getNameFromAccessID(fieldID) + "_rel_err_tol" +
        " = " + stencilInstantiation->getMetaData().getNameFromAccessID(fieldID) + "_rel_tol");
    runWrapper.addBodyLine("else");
    runWrapper.addBodyLine("  " + stencilInstantiation->getMetaData().getNameFromAccessID(fieldID) +
                           "_rel_err_tol" + " = DEFAULT_RELATIVE_ERROR_THRESHOLD");
    runWrapper.addBodyLine("endif");
    runWrapper.addBodyLine("");

    runWrapper.addBodyLine("if (present(" +
                           stencilInstantiation->getMetaData().getNameFromAccessID(fieldID) +
                           "_abs_tol" + ")) then");
    runWrapper.addBodyLine(
        "  " + stencilInstantiation->getMetaData().getNameFromAccessID(fieldID) + "_abs_err_tol" +
        " = " + stencilInstantiation->getMetaData().getNameFromAccessID(fieldID) + "_abs_tol");
    runWrapper.addBodyLine("else");
    runWrapper.addBodyLine("  " + stencilInstantiation->getMetaData().getNameFromAccessID(fieldID) +
                           "_abs_err_tol" + " = DEFAULT_ABSOLUTE_ERROR_THRESHOLD");
    runWrapper.addBodyLine("endif");
    runWrapper.addBodyLine("");
  }
  runWrapper.addACCLine("host_data use_device( &");
  auto fieldArgs = getFieldArgs(/*includeSavedState*/ true);
  for(int i = 0; i < fieldArgs.size(); ++i) {
    runWrapper.addACCLine(fortranIndent + fieldArgs[i] +
                          (i == (fieldArgs.size() - 1) ? " &" : ", &"));
  }
  runWrapper.addACCLine(")");
  runWrapper.addBodyLine("#ifdef __DSL_VERIFY", /*withIndentation*/ false);
  runWrapper.addBodyLine("call run_and_verify_" + stencilInstantiation->getName() + " &");
  genCallArgs(runWrapper, "", /*includeSavedState*/ true, /*includeErrorThreshold*/ true);
  runWrapper.addBodyLine("#else", /*withIndentation*/ false);
  runWrapper.addBodyLine("call run_" + stencilInstantiation->getName() + " &");
  genCallArgs(runWrapper, "", /*includeSavedState*/ false, /*includeErrorThreshold*/ false);
  runWrapper.addBodyLine("#endif", /*withIndentation*/ false);
  runWrapper.addACCLine("end host_data");

  fimGen.addWrapperAPI(std::move(runWrapper));

  std::vector<std::string> verticalBoundNames;
  for(auto fieldID : getUsedFields(stencil, {dawn::iir::Field::IntendKind::Output,
                                             dawn::iir::Field::IntendKind::InputOutput})) {
    verticalBoundNames.push_back(stencilInstantiation->getMetaData().getNameFromAccessID(fieldID) +
                                 "_kvert_max");
  }

  // memory management functions for production interface
  FortranInterfaceAPI setup("setup_" + stencilInstantiation->getName());
  FortranInterfaceAPI free("free_" + stencilInstantiation->getName());
  setup.addArg("mesh", FortranAPI::InterfaceType::OBJ);
  setup.addArg("k_size", FortranAPI::InterfaceType::INTEGER);
  setup.addArg("stream", FortranAPI::InterfaceType::CUDA_STREAM_T);
  for(auto fieldID : getUsedFields(stencil, {dawn::iir::Field::IntendKind::Output,
                                             dawn::iir::Field::IntendKind::InputOutput})) {
    setup.addArg(stencilInstantiation->getMetaData().getNameFromAccessID(fieldID) + "_kmax",
                 FortranAPI::InterfaceType::INTEGER);
  }

  fimGen.addInterfaceAPI(std::move(setup));
  fimGen.addInterfaceAPI(std::move(free));

  FortranWrapperAPI setupWrapper("wrap_setup_" + stencilInstantiation->getName());
  setupWrapper.addArg("mesh", FortranAPI::InterfaceType::OBJ);
  setupWrapper.addArg("k_size", FortranAPI::InterfaceType::INTEGER);
  setupWrapper.addArg("stream", FortranAPI::InterfaceType::CUDA_STREAM_T);

  for(auto fieldID : getUsedFields(stencil, {dawn::iir::Field::IntendKind::Output,
                                             dawn::iir::Field::IntendKind::InputOutput})) {
    setupWrapper.addOptArg(stencilInstantiation->getMetaData().getNameFromAccessID(fieldID) +
                               "_kmax",
                           FortranAPI::InterfaceType::INTEGER);
  }

  setupWrapper.addBodyLine("");

  for(int i = 0; i < verticalBoundNames.size(); i++) {
    setupWrapper.addBodyLine("integer(c_int) :: " + verticalBoundNames[i]);
  }

  setupWrapper.addBodyLine("");

  for(auto fieldID : getUsedFields(stencil, {dawn::iir::Field::IntendKind::Output,
                                             dawn::iir::Field::IntendKind::InputOutput})) {
    setupWrapper.addBodyLine("if (present(" +
                             stencilInstantiation->getMetaData().getNameFromAccessID(fieldID) +
                             "_kmax" + ")) then");
    setupWrapper.addBodyLine(
        "  " + stencilInstantiation->getMetaData().getNameFromAccessID(fieldID) + "_kvert_max" +
        " = " + stencilInstantiation->getMetaData().getNameFromAccessID(fieldID) + "_kmax");
    setupWrapper.addBodyLine("else");
    setupWrapper.addBodyLine("  " +
                             stencilInstantiation->getMetaData().getNameFromAccessID(fieldID) +
                             "_kvert_max" + " = k_size");
    setupWrapper.addBodyLine("endif");
    setupWrapper.addBodyLine("");
  }

  setupWrapper.addBodyLine("call setup_" + stencilInstantiation->getName() + " &");

  setupWrapper.addBodyLine("( &");
  setupWrapper.addBodyLine(fortranIndent + "mesh, &");
  setupWrapper.addBodyLine(fortranIndent + "k_size, &");
  setupWrapper.addBodyLine(fortranIndent + "stream, &");

  for(int i = 0; i < verticalBoundNames.size() - 1; i++) {
    setupWrapper.addBodyLine(fortranIndent + verticalBoundNames[i] + ", &");
  }

  setupWrapper.addBodyLine(fortranIndent + verticalBoundNames[verticalBoundNames.size() - 1] +
                           " &");

  setupWrapper.addBodyLine(")");

  fimGen.addWrapperAPI(std::move(setupWrapper));
}

std::string CudaIcoCodeGen::generateF90Interface(std::string moduleName) const {
  std::stringstream ss;
  ss << "#define DEFAULT_RELATIVE_ERROR_THRESHOLD 1.0d-12" << std::endl;
  ss << "#define DEFAULT_ABSOLUTE_ERROR_THRESHOLD 0.0d1" << std::endl;
  IndentedStringStream iss(ss);

  FortranInterfaceModuleGen fimGen(iss, moduleName);

  for(const auto& nameStencilCtxPair : context_) {
    std::shared_ptr<iir::StencilInstantiation> stencilInstantiation = nameStencilCtxPair.second;
    generateF90InterfaceSI(fimGen, stencilInstantiation);
  }

  fimGen.commit();

  return iss.str();
}

std::unique_ptr<TranslationUnit> CudaIcoCodeGen::generateCode() {
  DAWN_LOG(INFO) << "Starting code generation for ...";

  // Generate code for StencilInstantiations
  std::map<std::string, std::string> stencils;

  for(const auto& nameStencilCtxPair : context_) {
    std::shared_ptr<iir::StencilInstantiation> stencilInstantiation = nameStencilCtxPair.second;
    std::string code = generateStencilInstantiation(stencilInstantiation);
    if(code.empty())
      return nullptr;
    stencils.emplace(nameStencilCtxPair.first, std::move(code));
  }

  if(codeGenOptions_.OutputCHeader) {
    fs::path filePath = *codeGenOptions_.OutputCHeader;
    std::ofstream headerFile;
    headerFile.open(filePath);
    if(headerFile) {
      headerFile << generateCHeader();
      headerFile.close();
    } else {
      throw std::runtime_error("Error writing to " + filePath.string() + ": " + strerror(errno));
    }
  }

  if(codeGenOptions_.OutputFortranInterface) {
    fs::path filePath = *codeGenOptions_.OutputFortranInterface;
    std::string moduleName = filePath.filename().replace_extension("").string();
    std::ofstream interfaceFile;
    interfaceFile.open(filePath);
    if(interfaceFile) {
      interfaceFile << generateF90Interface(moduleName);
      interfaceFile.close();
    } else {
      throw std::runtime_error("Error writing to " + filePath.string() + ": " + strerror(errno));
    }
  }
  std::vector<std::string> ppDefines{
      "#include \"driver-includes/unstructured_interface.hpp\"",
      "#include \"driver-includes/unstructured_domain.hpp\"",
      "#include \"driver-includes/defs.hpp\"",
      "#include \"driver-includes/cuda_utils.hpp\"",
      "#include \"driver-includes/cuda_verify.hpp\"",
      "#include \"driver-includes/to_vtk.h\"",
      "#define GRIDTOOLS_DAWN_NO_INCLUDE", // Required to not include gridtools from math.hpp
      "#include \"driver-includes/math.hpp\"",
      "#include <chrono>",
      "#define BLOCK_SIZE " + std::to_string(codeGenOptions_.BlockSize),
      "#define LEVELS_PER_THREAD " + std::to_string(codeGenOptions_.LevelsPerThread),
      "using namespace gridtools::dawn;",
  };

  std::string globals = generateGlobals(context_, "dawn_generated", "cuda_ico");

  DAWN_LOG(INFO) << "Done generating code";

  std::string filename = generateFileName(context_);

  return std::make_unique<TranslationUnit>(filename, std::move(ppDefines), std::move(stencils),
                                           std::move(globals));
}
} // namespace cudaico
} // namespace codegen
} // namespace dawn

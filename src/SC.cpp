#include "function-filter/Filter.hpp"
#include "function-filter/Marker.hpp"
#include "input-dependency/Analysis/FunctionInputDependencyResultInterface.h"
#include "input-dependency/Analysis/InputDependencyAnalysisPass.h"
#include "self-checksumming/DAGCheckersNetwork.h"
#include "self-checksumming/Stats.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include <composition/graph/constraint/dependency.hpp>
#include <composition/graph/constraint/present.hpp>
#include <composition/support/Analysis.hpp>
#include <cxxabi.h>
#include <limits.h>
#include <random>
#include <sstream>
#include <stdint.h>

using namespace llvm;
using namespace composition;

static cl::opt<bool> UseOtherFunctions(
    "use-other-functions", cl::Hidden,
    cl::desc("This allows SC to use other functions, beyond the specified "
             "filter set, as checkers to meet the desired connectivity level"));

static cl::opt<bool> SensitiveOnlyChecked(
    "sensitive-only-checked", cl::Hidden,
    cl::desc(
        "Sensitive functions are only checked and never used as checkers. "
        "Extracted only always is given higher priority, that is sensitive "
        "functions are never checkers when extracted only is set"));

static cl::opt<bool> ExtractedOnly(
    "extracted-only", cl::Hidden,
    cl::desc(
        "Only extracted functions are protected using SC, extracted functions "
        "in are always checkees and never checkers, this mode uses other "
        "functions regardless of setting use-other-functions flag or not. "));

static cl::opt<int> DesiredConnectivity(
    "connectivity", cl::Hidden,
    cl::desc(
        "The desired level of connectivity of checkers node in the network "));

static cl::opt<int> MaximumPercOtherFunctions(
    "maximum-other-percentage", cl::Hidden,
    cl::desc("The maximum usage percentage (between 0 and 100) of other "
             "functions (beyond the filter set) that should be also "
             "included in the SC protection "));

static cl::opt<std::string> LoadCheckersNetwork(
    "load-checkers-network", cl::Hidden,
    cl::desc("File path to load checkers' network in Json format "));

static cl::opt<std::string>
    DumpSCStat("dump-sc-stat", cl::Hidden,
               cl::desc("File path to dump pass stat in Json format "));

static cl::opt<std::string> DumpCheckersNetwork(
    "dump-checkers-network", cl::Hidden,
    cl::desc("File path to dump checkers' network in Json format "));

namespace {

std::string demangle_name(const std::string &name) {
  int status = -1;
  char *demangled =
      abi::__cxa_demangle(name.c_str(), nullptr, nullptr, &status);
  if (status != 0) {
    return name;
  }
  std::string demangled_name(demangled);
  demangled_name.erase(
      std::remove(demangled_name.begin(), demangled_name.end(), ' '),
      demangled_name.end());
  for (char &c : demangled_name) {
    if (c == '(' || c == '*' || c == '&' || c == ')' || c == ',' || c == '<' ||
        c == '>' || c == '~' || c == '[' || c == ']') {
      c = '_';
    }
  }
  return demangled_name;
}

struct SCPass : public composition::support::ComposableAnalysis<SCPass> {
  Stats stats;
  static char ID;

  SCPass() = default;

  llvm::MDNode *sc_guard_md{};
  const std::string sc_guard_str = "sc_guard";

  // Stats function list
  std::map<Function *, int> ProtectedFuncs;
  int numberOfGuards = 0;
  int numberOfGuardInstructions = 0;
  std::vector<Function *> sensitiveFunctions;

  /*long getFuncInstructionCount(const Function &F){
      long count=0;
      for (BasicBlock& bb : F){
        count += std::distance(bb.begin(), bb.end());
      }
      return count;
  }*/

  bool assert_sensitive_only_checked_condition(
      const std::vector<Function *> sensitiveFunctions,
      const std::map<Function *, std::vector<Function *>> &checkerFuncMap) {
    for (auto &func : sensitiveFunctions) {
      if (checkerFuncMap.find(func) != checkerFuncMap.end()) {
        errs() << "Sensitive functions are checkers while SensitiveOnlyChecked "
                  "is set to:"
               << SensitiveOnlyChecked << "\n";
        exit(1);
      }
    }
    dbgs() << "Sensitive functions are never checkers as SensitiveOnlyChecked "
              "is set to:"
           << SensitiveOnlyChecked << "\n";
    return true;
  }

  bool runOnModule(Module &M) override {
    bool didModify = false;
    std::vector<Function *> otherFunctions;
    const auto &input_dependency_info =
        getAnalysis<input_dependency::InputDependencyAnalysisPass>()
            .getInputDependencyAnalysis();
    auto *function_info =
        getAnalysis<FunctionMarkerPass>().get_functions_info();
    auto function_filter_info =
        getAnalysis<FunctionFilterPass>().get_functions_info();

    auto *sc_guard_md_str = llvm::MDString::get(M.getContext(), sc_guard_str);
    sc_guard_md = llvm::MDNode::get(M.getContext(), sc_guard_md_str);

    int countProcessedFuncs = 0;
    for (auto &F : M) {
      if (F.isDeclaration() || F.empty() || F.getName() == "guardMe")
        continue;

      countProcessedFuncs++;
      auto F_input_dependency_info = input_dependency_info->getAnalysisInfo(&F);

      // TODO: Why skipping such functions?
      if (!F_input_dependency_info) {
        dbgs() << "Skipping function because it has no input dependency result "
               << F.getName() << "\n";
        continue;
      }
      bool isExtracted = F_input_dependency_info->isExtractedFunction();
      bool isSensitive =
          ExtractedOnly
          ? isExtracted
          : true; // only extracted functions if ExtarctedOnly is set
      // honor the filter function list
      if (!function_filter_info->get_functions().empty() &&
          !function_filter_info->is_function(&F)) {
        isSensitive = false;
      }
      // todo otherFunctions是啥
      // ExtractedOnly flag enforces the usage of other functions, regardless of
      // the UseOtherFunctions flag
      if (ExtractedOnly && (!isExtracted)) {
        dbgs() << "Adding " << F.getName()
               << " other functions, ExtractedOnly mode uses other functions\n";
        otherFunctions.push_back(&F);
      } else if (!ExtractedOnly && UseOtherFunctions && !isSensitive) {
        dbgs() << "Adding " << F.getName()
               << " other functions, UseOtherFunctions mode\n";
        otherFunctions.push_back(&F);
      } else if (F.getName() == "main") {
        otherFunctions.push_back(&F);
      } else if (isSensitive) {
        dbgs() << "Adding " << F.getName() << " to sensitive vector\n";
        sensitiveFunctions.push_back(&F);
      }
    }

    // auto rd = std::random_device{};
    // auto rng = std::default_random_engine{rd()};
    auto rng = std::default_random_engine{};

    dbgs() << "Sensitive functions:" << sensitiveFunctions.size()
           << " other functions:" << otherFunctions.size() << "\n";
    // shuffle all functions
    std::shuffle(std::begin(sensitiveFunctions), std::end(sensitiveFunctions),
                 rng);
    std::shuffle(std::begin(otherFunctions), std::end(otherFunctions), rng);

    if (DesiredConnectivity == 0) {
      DesiredConnectivity = 2;
    }
    dbgs() << "DesiredConnectivity is :" << DesiredConnectivity << "\n";

    // Implement #43
    /*int totalNodes = sensitiveFunctions.size() + DesiredConnectivity;
    int actual_connectivity = DesiredConnectivity;
    bool accept_lower_connectivity = false;
    //make sure we can satisfy this requirement, i.e. we have enough functions
    if (DesiredConnectivity > otherFunctions.size()){
        //adjust actual connectivity
        dbgs()<<"SCPass. There is not enough functions in the module to satisfy
    the desired connectivity...\n";
        //TODO: decide whether carrying on or downgrading connectivity is better
        //actual_connectivity =
    otherFunctions.size()+availableInputIndependents;
        //dbgs()<<"Actual connectivity is:"<<actual_connectivity<<"\n";
        dbgs()<<"Carrying on with the desired connectivity nonetheless";
        accept_lower_connectivity = true;
    }*/

    // dbgs() << "Total nodes:" << totalNodes << "\n";
    // int availableOtherFunction = 0;
    // indicates that we need to take some other functions
    // availableOtherFunction = actual_connectivity;
    // dbgs() << "available other functions:" << availableOtherFunction << "\n";

    // if (availableOtherFunction > 0) {
    // for (Function *func : otherFunctions) {
    // dbgs() << "pushing back other input dependent function "
    //       << func->getName() << "\n";
    // allFunctions.push_back(func);
    // availableOtherFunction--;
    // if (availableOtherFunction <= 0)
    //  break;
    // }
    // }

    dbgs() << "Other functions to be fed to the network of checkers\n";
    for (auto &F : otherFunctions) {
      dbgs() << F->getName() << "\n";
    }
    dbgs() << "***\n";
    dbgs() << "Sensitive functions to be fed to the network of checkers\n";
    for (auto &F : sensitiveFunctions) {
      dbgs() << F->getName() << "\n";
    }
    dbgs() << "***\n";
    dbgs() << "Sensitive functions only checked:" << SensitiveOnlyChecked
           << "\n";

    DAGCheckersNetwork checkerNetwork;
    checkerNetwork.setLowerConnectivityAcceptance(true);
    // map functions to checker checkee map nodes
    std::list<Function *> topologicalSortFuncs;
    std::map<Function *, std::vector<Function *>> checkerFuncMap;
    std::vector<int> actucalConnectivity;
    if (!LoadCheckersNetwork.empty()) {
//        在程序运行时，可以通过命令行参数 -input=filename 来指定输入文件名。使用 InputFileName.getValue() 来获取该选项的值，并输出到控制台。
      checkerFuncMap = checkerNetwork.loadJson(LoadCheckersNetwork.getValue(),
                                               M, topologicalSortFuncs);
      if (!DumpSCStat.empty()) {
        // TODO: maybe we dump the stats into the JSON file and reload it just
        // like the network
        errs() << "ERR. Stats is not avalilable for the loaded networks...";
        exit(1);
      }
    } else {
      if (!SensitiveOnlyChecked &&
          !ExtractedOnly) // SensitiveOnlyChecked prevents sensitive function
        // being picked as checkers, extracted functions are
        // never checkers
      {
        otherFunctions.insert(otherFunctions.end(), sensitiveFunctions.begin(),
                              sensitiveFunctions.end());
      }
      checkerFuncMap = checkerNetwork.constructProtectionNetwork(
          sensitiveFunctions, otherFunctions, DesiredConnectivity);
      topologicalSortFuncs =
          checkerNetwork.getReverseTopologicalSort(checkerFuncMap);
      dbgs() << "Constructed the network of checkers!\n";
      if (SensitiveOnlyChecked || ExtractedOnly) {
        assert_sensitive_only_checked_condition(sensitiveFunctions,
                                                checkerFuncMap);
      }
    }
    if (!DumpCheckersNetwork.empty()) {
      dbgs() << "Dumping checkers network info\n";
      checkerNetwork.dumpJson(checkerFuncMap, DumpCheckersNetwork.getValue(),
                              topologicalSortFuncs);
    } else {
      dbgs() << "No checkers network info file is requested!\n";
    }
    unsigned int marked_function_count = 0;

    // Fix for issue #58
    for (auto &SF : sensitiveFunctions) {
      ProtectedFuncs[SF] = 0;
    }

    // inject one guard for each item in the checkee vector
    // reverse topologically sorted
    // todo 为什么要逆序
//      这段代码是在一个循环中为每个检查器函数中的每个被保护函数注入一条保护指令。
//
//      循环的过程如下：
//      1. 对于拓扑排序后的每个检查器函数 `F`，从 `checkerFuncMap` 中查找它对应的被保护函数列表 `it->second`。
//      2. 对于每个被保护函数 `Checkee`，进行以下操作：
//      a. 调用 `injectGuard` 函数，为被保护函数 `Checkee` 在检查器函数 `F` 的入口基本块中插入一条保护指令。该函数返回一个元组 `std::tuple`，其中第一个元素 `undoValues` 是需要撤销保护的值的集合，第二个元素 `_patchFunction` 是用于撤销保护的函数。
//      b. 为了避免 Clang 编译器的一个 bug，将 `_patchFunction` 赋值给一个名为 `patchFunction` 的变量。
//      c. 定义 `redo` 函数，该函数会在保护被撤销时执行。在该函数中，进行以下操作：
//      - 如果被保护函数 `Checkee` 在敏感函数列表 `sensitiveFunctions` 中，则将 `Checkee` 对应的值在 `ProtectedFuncs` 中的计数加1，并在 `FunctionMarkerPass` 中记录 `Checkee`。
//      - 更新 `marked_function_count` 变量，表示已标记的函数数量加1。
//      - 在调试输出中记录成功在检查器函数 `F` 中为被保护函数 `Checkee` 插入了保护指令。
//      - 将 `numberOfGuards` 计数加1，表示成功插入了一条保护指令。
//      - 调用 `patchFunction`，将 `m` 作为参数传递，撤销保护并执行特定的操作。
//      d. 创建一个 `undoValueSet` 集合，用于存储需要撤销保护的值。
//      e. 将 `undoValues` 中的元素添加到 `undoValueSet` 中。
//      f. 创建一个名为 `m` 的 `Manifest` 对象，用于描述保护的属性。该对象包含保护类型、被保护函数指针、重做函数 `redo`、约束条件等信息。
//      g. 将 `m` 添加到保护列表中，表示为被保护函数 `Checkee` 添加了保护。
//      h. 将 `didModify` 设置为 `true`，表示成功修改了函数模块。
//
//      循环执行完毕后，每个检查器函数中的被保护函数都成功插入了一条保护指令，并且相关的统计信息和保护列表都已更新。
    for (auto &F : topologicalSortFuncs) {
      auto it = checkerFuncMap.find(F);
      if (it == checkerFuncMap.end())
        continue;
      auto &BB = F->getEntryBlock();
//        函数的作用是返回当前基本块（BasicBlock）中第一个非 PHI 节点（指令）或调试信息节点（DbgNode）的指针。这个函数用于遍历基本块的指令，并跳过所有的 PHI 节点和调试信息节点，直到找到第一个非 PHI 节点或调试信息节点为止。
      auto I = BB.getFirstNonPHIOrDbg();

      auto F_input_dependency_info = input_dependency_info->getAnalysisInfo(F);
      for (auto &Checkee : it->second) {
        assert(it->first != nullptr && "IT First is nullptr");
        assert(Checkee != nullptr && "Checkee is nullptr");

        auto[undoValues, _patchFunction] = injectGuard(
            &BB, I, Checkee, numberOfGuardInstructions,
            false); // F_input_dependency_info->isInputDepFunction() ||
        // F_input_dependency_info->isExtractedFunction());

        // Clang compiler bug otherwise
        auto patchFunction = _patchFunction;
        auto redo = [Checkee, function_info, &marked_function_count, F,
            patchFunction, this](const Manifest &m) {
          // This is all for the sake of the stats
          // only collect connectivity info for sensitive functions
          if (std::find(sensitiveFunctions.begin(), sensitiveFunctions.end(),
                        Checkee) != sensitiveFunctions.end())
            ++ProtectedFuncs[Checkee];
          // End of stats
          // Note checkees in Function marker pass
          function_info->add_function(Checkee);
          marked_function_count++;

          dbgs() << "Insert guard in " << F->getName()
                 << " checkee: " << Checkee->getName() << "\n";
          numberOfGuards++;

          patchFunction(m);
        };

        std::set<llvm::Value *> undoValueSet{};
        for (auto u : undoValues) {
          undoValueSet.insert(u);
        }

        auto m = new Manifest(
            "sc", Checkee, nullptr, redo,
            {std::make_unique<graph::constraint::Dependency>("sc", it->first,
                                                             Checkee),
             std::make_unique<graph::constraint::Present>("sc", Checkee)},
            true, undoValueSet, patchInfo);
        addProtection(m);

        didModify = true;
      }
    }

    // assertFilteredMarked(function_filter_info, countProcessedFuncs,
    // marked_function_count);
    return didModify;
  }

  bool doFinalization(Module &module) override;

  void assertFilteredMarked(const FunctionInformation *function_filter_info,
                            int countProcessedFuncs,
                            unsigned int marked_function_count) const {
    const auto &funinfo =
        getAnalysis<FunctionMarkerPass>().get_functions_info();
    dbgs() << "Recieved marked functions " << funinfo->get_functions().size()
           << "\n";
    if (marked_function_count != funinfo->get_functions().size()) {
      dbgs() << "ERR. Marked functions " << marked_function_count
             << " are not reflected correctly "
             << funinfo->get_functions().size() << "\n";
    }
    // Make sure OH only processed filter function list
    if (countProcessedFuncs != function_filter_info->get_functions().size() &&
        !function_filter_info->get_functions().empty()) {
      errs() << "ERR. processed " << countProcessedFuncs
             << " function, while filter count is "
             << function_filter_info->get_functions().size() << "\n";
      // exit(1);
    }
  }

  void dumpStats(const std::vector<Function *> &sensitiveFunctions,
                 const std::map<Function *, int> &ProtectedFuncs,
                 int numberOfGuards,
                 int numberOfGuardInstructions) { // Do we need to dump stats?
    if (!DumpSCStat.empty()) {
      // calc number of sensitive instructions
      long sensitiveInsts = 0;
      for (const auto &function : sensitiveFunctions) {
        for (BasicBlock &bb : *function) {
          sensitiveInsts += std::distance(bb.begin(), bb.end());
        }
      }
      stats.setNumberOfSensitiveInstructions(sensitiveInsts);
      stats.addNumberOfGuards(numberOfGuards);
      stats.addNumberOfProtectedFunctions(
          static_cast<int>(ProtectedFuncs.size()));
      stats.addNumberOfGuardInstructions(numberOfGuardInstructions);
      stats.setDesiredConnectivity(DesiredConnectivity);
      long protectedInsts = 0;
      std::vector<int> frequency;

      for (const auto &item : ProtectedFuncs) {
        const auto &function = item.first;
        const int frequencyOfChecks = item.second;
        for (BasicBlock &bb : *function) {
          protectedInsts += std::distance(bb.begin(), bb.end());
        }
        frequency.push_back(frequencyOfChecks);
      }
      stats.addNumberOfProtectedInstructions(protectedInsts);
      stats.calculateConnectivity(frequency);
      // stats.setAvgConnectivity(actual_connectivity);
      // stats.setStdConnectivity(0);
      dbgs() << "SC stats is requested, dumping stat file...\n";
      stats.dumpJson(DumpSCStat.getValue());
    }
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesAll();
    AU.addRequired<input_dependency::InputDependencyAnalysisPass>();
    AU.addRequired<FunctionMarkerPass>();
    AU.addPreserved<FunctionMarkerPass>();
    AU.addRequired<FunctionFilterPass>();
    AU.addPreserved<FunctionFilterPass>();
  }

  uint64_t rand_uint64() {
    uint64_t r = 0;
    for (int i = 0; i < 64; i += 30) {
      r = r * ((uint64_t) RAND_MAX + 1) + rand();
    }
    return r;
  }

  void appendToPatchGuide(const unsigned int length, const unsigned int address,
                          const unsigned int expectedHash,
                          const std::string &functionName) {
    FILE *pFile;
    pFile = fopen("guide.txt", "a");
    std::string demangled_name = demangle_name(functionName);
    fprintf(pFile, "%s,%d,%d,%d\n", demangled_name.c_str(), address, length,
            expectedHash);
    fclose(pFile);
  }

  void setPatchMetadata(Instruction *Inst, const std::string &tag) {
    LLVMContext &C = Inst->getContext();
    MDNode *N = MDNode::get(C, MDString::get(C, tag));
    Inst->setMetadata("guard", N);
  }

  unsigned int size_begin = 555555555;
  unsigned int address_begin = 222222222;
  unsigned int expected_hash_begin = 444444444;

//    这段代码定义了一个名为 `injectGuard` 的函数，该函数用于在给定基本块 (`BasicBlock`) 和指令 (`Instruction`) 的位置插入保护代码。函数的主要功能是创建一个新的函数调用指令，称为 "guard"，用于保护特定的函数 (`Checkee`)。
//
//    具体来说，`injectGuard` 函数的参数包括基本块 `BB`，指令 `I`，要保护的函数 `Checkee`，还有一些额外的参数。函数首先获取上下文 (context) 和模块 (module) 的相关信息，然后创建一个名为 "guardMe" 的函数，如果该函数已存在则直接获取。接着，根据传入的 `is_in_inputdep` 参数，确定 `args` 和 `undoValues` 这两个向量的内容。
//
//    `args` 向量存储着用于调用 "guardMe" 函数的参数，根据 `is_in_inputdep` 的值，可能只包含 `address`、`length` 和 `expectedHash` 三个参数，或者还包含一些额外的参数。`undoValues` 向量用于存储插入的保护代码，这些代码在后续需要撤销保护时会用到。
//
//    接下来，函数通过 `IRBuilder` 在指定位置插入代码，包括存储参数、加载参数、调用 "guardMe" 函数等操作，并将相关的指令和值添加到 `undoValues` 中。同时，函数还生成用于统计的 `patchInfo` 字符串。
//
//    最后，函数返回一个包含两个元素的 `std::pair`，其中第一个元素是 `undoValues` 向量，第二个元素是一个 lambda 函数 `patchFunction`。`patchFunction` 函数用于撤销保护时执行相应的操作，例如更新统计信息、移除标记以及给被保护的函数添加属性。
//
//    总体而言，`injectGuard` 函数是用于生成并插入保护代码的功能性函数，以实现对特定函数的保护和保护后的撤销。

//        `BasicBlock` 是 LLVM 中的基本块，是 LLVM 中的基本组成单元之一。一个基本块是一个连续的指令序列，其中的指令按照控制流顺序执行，没有分支或跳转语句。基本块通常由一个入口指令（首指令）和一个终止指令（尾指令）组成。
//
//        在 LLVM 的中间表示 (Intermediate Representation, IR) 中，函数被表示为一个由基本块组成的控制流图 (Control Flow Graph, CFG)。每个基本块都有一个唯一的标识符，可以通过这个标识符来访问基本块。
//
//        基本块在 LLVM 中扮演了重要的角色，它们是构建程序的基本模块，用于表示不同的代码路径和执行顺序。在优化和分析过程中，基本块是重要的单位，许多优化算法都是基于对基本块的分析和转换。
//
//        在给定函数的控制流图中，每个基本块都是一个独立的代码段，由一组指令组成。基本块之间通过跳转指令（如条件分支或无条件分支）进行连接，形成函数的控制流程。
//
//        总结来说，`BasicBlock` 是 LLVM 中表示函数控制流图中基本代码块的数据结构，它包含一组按照控制流顺序执行的指令。
  std::pair<std::vector<llvm::Value *>, PatchFunction>
  injectGuard(BasicBlock *BB, Instruction *I, Function *Checkee,
              int &numberOfGuardInstructions, bool is_in_inputdep) {
    LLVMContext &Ctx = BB->getParent()->getContext();
    // get BB parent -> Function -> get parent -> Module
    llvm::ArrayRef<llvm::Type *> params;
    params = {Type::getInt32Ty(Ctx), Type::getInt32Ty(Ctx),
              Type::getInt32Ty(Ctx)};

//            这行代码用于创建一个 LLVM 函数类型对象 (`llvm::FunctionType`)。函数类型描述了函数的参数类型和返回类型。
//
//            函数签名如下：
//            ```cpp
//            static FunctionType *get(Type *Result, ArrayRef<Type *> Params, bool isVarArg);
//            ```
//
//            参数解释：
//            - `Result`: 表示函数的返回类型，这里使用 `llvm::Type::getVoidTy(Ctx)` 表示返回类型为 `void`，即函数没有返回值。
//            - `Params`: 一个 `ArrayRef`，表示函数的参数类型。`params` 是一个类型为 `llvm::Type *` 的 `ArrayRef`，它包含了函数的参数类型。在这段代码中，`params` 可能包含多种类型，具体取决于代码上下文中的实际参数列表。
//            - `isVarArg`: 表示函数是否是可变参数函数（即带有 `...` 的函数），这里设置为 `false`，表示不是可变参数函数。
//
//            所以，这行代码创建了一个函数类型对象 `function_type`，它表示一个没有返回值且参数类型由 `params` 指定的函数类型。在这里，函数类型是一个没有返回值和参数的函数类型，即类似于 C/C++ 中的 `void func()`。
//
//            LLVM 中的函数类型用于描述函数的签名，包括返回类型和参数类型，这在 LLVM IR 中的函数定义和调用中非常重要。

 // 表示guardme的函数类型
    llvm::FunctionType *function_type =
        llvm::FunctionType::get(llvm::Type::getVoidTy(Ctx), params, false);

//`getOrInsertFunction` 是 LLVM 中的一个函数，在 `llvm/IR/Module.h` 头文件中声明，用于在模块中获取或插入一个函数。
//
//函数签名如下：
//```cpp
//FunctionCallee getOrInsertFunction(StringRef Name, FunctionType *T);
//```
//
//参数解释：
//- `Name`：表示要获取或插入的函数的名称，为一个字符串引用。
//- `T`：表示要获取或插入的函数的函数类型（`llvm::FunctionType *`）。函数类型描述了函数的参数类型和返回类型，前面提到的 `llvm::FunctionType::get` 就是用于创建函数类型的。
//
//函数返回值：
//- `FunctionCallee`：一个包装了函数指针或函数地址的类，用于表示函数的指针。这个类是一个通用的函数指针包装器，可以表示不同类型的函数指针。
//
//函数作用：
//- 如果在模块中找到了与给定名称和类型匹配的函数，则返回一个 `FunctionCallee` 对象，该对象包含了已存在函数的指针。
//- 如果模块中没有找到与给定名称和类型匹配的函数，则在模块中插入一个新的函数，并返回一个 `FunctionCallee` 对象，该对象包含了新函数的指针。
//
//`getOrInsertFunction` 在 LLVM IR 中的使用场景是在进行函数调用之前，先检查是否已经有匹配的函数定义，如果有则直接获取函数指针，如果没有则插入一个新的函数定义。这样可以确保在生成 LLVM IR 代码时，所有被调用的函数都能正确链接。

//执行 `->getParent()->getParent()` 是为了从 BasicBlock (`BB`) 获取到所属的 Function，然后再从 Function 获取到所属的 Module。
//
//在 LLVM IR 中，一个 BasicBlock 属于一个 Function，而一个 Function 又属于一个 Module。所以在这段代码中，首先通过 `BB->getParent()` 获取到 BasicBlock 所属的 Function，然后再通过 `->getParent()` 获取到 Function 所属的 Module。
//
//具体来说，`BB->getParent()` 返回一个指向所属 Function 的指针，而 `->getParent()` 再次调用此指针所指向的 Function 对象的 `getParent()` 函数，返回一个指向所属 Module 的指针。最终，通过两次 `getParent()` 操作，可以得到 BasicBlock 所属的 Module。
//
//因此，`BB->getParent()->getParent()` 的目的是为了从 BasicBlock 获取到其所属的 Module，以便后续的操作可以在正确的 Module 中进行。


//            注意，这个方法并不会生成函数的实际定义体（即函数的具体实现），它只是在模块中声明了一个函数。如果需要为函数生成实际的定义体，需要在其他地方进行函数的定义和实现。
Constant *guardFunc = BB->getParent()->getParent()->getOrInsertFunction(
        "guardMe", function_type);// todo 这个函数有谁调用

    IRBuilder<> builder(I);
    auto insertPoint = ++builder.GetInsertPoint();
    if (llvm::isa<TerminatorInst>(I)) {
      insertPoint--;
    }
    builder.SetInsertPoint(BB, insertPoint);
    unsigned int length = size_begin++;
    unsigned int address = address_begin++;
    unsigned int expectedHash = expected_hash_begin++;

    std::vector<llvm::Value *> args;

    auto *arg1 = llvm::ConstantInt::get(llvm::Type::getInt32Ty(Ctx), address);
    auto *arg2 = llvm::ConstantInt::get(llvm::Type::getInt32Ty(Ctx), length);
    auto *arg3 =
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(Ctx), expectedHash);

    std::vector<llvm::Value *> undoValues{};

    undoValues.push_back(arg1);
    undoValues.push_back(arg2);
    undoValues.push_back(arg3);
    int localGuardInstructions;
    if (is_in_inputdep) {
      args.push_back(arg1);
      args.push_back(arg2);
      args.push_back(arg3);
      localGuardInstructions = 1;
    } else {
      auto *A = builder.CreateAlloca(Type::getInt32Ty(Ctx), nullptr, "a");
      auto *B = builder.CreateAlloca(Type::getInt32Ty(Ctx), nullptr, "b");
      auto *C = builder.CreateAlloca(Type::getInt32Ty(Ctx), nullptr, "c");
      auto *store1 = builder.CreateStore(arg1, A, /*isVolatile=*/false);
      store1->setMetadata(sc_guard_str, sc_guard_md);
      // setPatchMetadata(store1, "address");
      auto *store2 = builder.CreateStore(arg2, B, /*isVolatile=*/false);
      store2->setMetadata(sc_guard_str, sc_guard_md);
      // setPatchMetadata(store2, "length");
      auto *store3 = builder.CreateStore(arg3, C, /*isVolatile=*/false);
      store3->setMetadata(sc_guard_str, sc_guard_md);
      // setPatchMetadata(store3, "hash");
      auto *load1 = builder.CreateLoad(A);
      load1->setMetadata(sc_guard_str, sc_guard_md);
      auto *load2 = builder.CreateLoad(B);
      load2->setMetadata(sc_guard_str, sc_guard_md);
      auto *load3 = builder.CreateLoad(C);
      load3->setMetadata(sc_guard_str, sc_guard_md);
      args.push_back(load1);
      args.push_back(load2);
      args.push_back(load3);

      undoValues.push_back(A);
      undoValues.push_back(B);
      undoValues.push_back(C);
      undoValues.push_back(store1);
      undoValues.push_back(store2);
      undoValues.push_back(store3);
      undoValues.push_back(load1);
      undoValues.push_back(load2);
      undoValues.push_back(load3);

      localGuardInstructions = 9;
    }

    CallInst *call = builder.CreateCall(guardFunc, args);
    call->setMetadata(sc_guard_str, sc_guard_md);
    undoValues.push_back(call);
    setPatchMetadata(call, Checkee->getName());
    // Stats: we assume the call instrucion and its arguments account for one
    // instruction
    std::ostringstream patchInfoStream{};
    std::string demangled_name = demangle_name(Checkee->getName());
    patchInfoStream << demangled_name.c_str() << "," << address << "," << length
                    << "," << expectedHash << "\n";
    patchInfo = patchInfoStream.str();

    auto patchFunction = [length, address, expectedHash, arg1, arg2, arg3,
        localGuardInstructions, &numberOfGuardInstructions,
        Checkee, this](const Manifest &m) {
      dbgs() << "placeholder:" << address << " size:" << length
             << " expected hash:" << expectedHash << "\n";
      appendToPatchGuide(length, address, expectedHash, Checkee->getName());
      addPreserved("sc", arg1,
                   [this](const std::string &pass, llvm::Value *oldV,
                          llvm::Value *newV) { assert(false); });
      addPreserved("sc", arg2,
                   [this](const std::string &pass, llvm::Value *oldV,
                          llvm::Value *newV) { assert(false); });
      addPreserved("sc", arg3,
                   [this](const std::string &pass, llvm::Value *oldV,
                          llvm::Value *newV) { assert(false); });
      numberOfGuardInstructions += localGuardInstructions;
      Checkee->addFnAttr(llvm::Attribute::NoInline);
    };

    return {undoValues, patchFunction};
  }

  std::string patchInfo;
};
} // namespace

char SCPass::ID = 0;

bool SCPass::doFinalization(Module &module) {
  dumpStats(sensitiveFunctions, ProtectedFuncs, numberOfGuards,
            numberOfGuardInstructions);

  return ModulePass::doFinalization(module);
}

static llvm::RegisterPass<SCPass> X("sc", "Instruments bitcode with guards",
                                    true, false);

#include "ICAnalyses.hpp"
#include <fstream>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/CodeGen/MachineBasicBlock.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/Transforms/Utils/ValueMapper.h>

#include "llvm/Transforms/Utils/ScalarEvolutionExpander.h"

#include "llvm/Support/TargetSelect.h"      // InitializeNativeTarget
#include "llvm/MC/TargetRegistry.h"         // TargetRegistry
#include "llvm/TargetParser/Host.h"         // getDefaultTargetTriple
#include "llvm/Target/TargetMachine.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

using llvm::yaml::IO;

void yaml::MappingTraits<IC::Config>::mapping(IO &io, IC::Config &config) {
  io.mapRequired("energy_model_names", config.energy_model_names);
  io.mapRequired("instructions_to_count", config.instructions_to_count);
  io.mapRequired("targets_allowed", config.targets);
  io.mapOptional("aggregation_level", config.aggregate_level, IC::Config::FID);
  io.mapOptional("verbose", config.verbose, false);
  io.mapOptional("run_tests", config.run_tests, false);
}

void yaml::ScalarEnumerationTraits<IC::Config::AggregationLevel>::enumeration(
    IO &io, IC::Config::AggregationLevel &value) {
  io.enumCase(value, "fid", IC::Config::FID);
  io.enumCase(value, "constants", IC::Config::Constants);
  io.enumCase(value, "all", IC::Config::All);
}

namespace IC {

AnalysisKey CounterFunctionAnalysis::Key;
AnalysisKey CountAggregationFunctionAnalysis::Key;
AnalysisKey CounterModuleAnalysis::Key;
AnalysisKey ConfigReader::Key;

bool Config::isTargetValid(const Triple &target) {
  return true;
  for (auto &t : targets) {
    std::string tl{t};
    std::transform(tl.begin(), tl.end(), tl.begin(), ::tolower);
    if (tl == "gpu") {
      if (target.isNVPTX() || target.isAMDGPU())
        return true;
    } else if (tl == "nvptx") {
      if (target.isNVPTX())
        return true;
    } else if (tl == "amdgpu") {
      if (target.isAMDGPU())
        return true;
    } else if (tl == "spir-v") {
      if (target.isSPIRV())
        return true;
    } else if (tl == "spir") {
      if (target.isSPIR())
        return true;
    } else if (tl == "dxil") {
      if (target.isDXIL())
        return true;
    } else if (tl == "cpu") {
      if (target.isNVPTX() || target.isAMDGPU())
        return true;
    } else if (tl == "riscv") {
      if (target.isRISCV())
        return true;
    } else if (tl == "riscv32") {
      if (target.isRISCV32())
        return true;
    } else if (tl == "riscv64") {
      if (target.isRISCV64())
        return true;
    } else if (tl == "arm") {
      if (target.isARM())
        return true;
    } else if (tl == "x86") {
      if (target.isX86())
        return true;
    }
  }
  return false;
}

bool Config::invalidate(Module &M, const PreservedAnalyses &PA,
                        ModuleAnalysisManager::Invalidator &Invalidator) {
  auto PAC = PA.getChecker(&ConfigReader::Key);
  return !PAC.preservedWhenStateless();
}
void ConfigReader::loadEnergyModel(Config &config,
                                   std::filesystem::path base_directory,
                                   std::string energy_model_name) {
  std::ifstream energy_model_file;
  std::filesystem::path energy_model_dir_path =
      base_directory / "energy_models";
  std::filesystem::path energy_model_file_path =
      energy_model_dir_path / (energy_model_name + ".txt");

  energy_model_file.open(energy_model_file_path);
  if (!energy_model_file.is_open()) {
    errs() << "Error opening energy model file at " << energy_model_file_path
           << "\n";
    return;
  }

  std::ostringstream osstr;
  osstr << energy_model_file.rdbuf();
  energy_model_file.close();
  std::string energy_model_contents = osstr.str();

  std::string line;
  std::istringstream isstr{energy_model_contents};
  while (std::getline(isstr, line)) {
    StringRef lineref = line;
    std::size_t colon_pos = lineref.find(":", 0);
    if (colon_pos == std::string::npos) {
      errs() << "Line \"" << line << "\" in energy model file at "
             << energy_model_file_path << " is malformed. Skipping\n";
      continue;
    }
    StringRef instruction_name = lineref.substr(0, colon_pos).trim();
    StringRef energy_usage_str = lineref.substr(colon_pos + 1).trim();
    if (energy_usage_str.empty()) {
      errs() << "Line \"" << line << "\" in energy model file at "
             << energy_model_file_path << "is malformed. Skipping\n";
      continue;
    }
    std::size_t energy_usage;
    if (!llvm::to_integer<std::size_t>(StringRef(energy_usage_str).trim(),
                                       energy_usage)) {
      errs() << "Error while parsing line: \"" << line << "\" in "
             << energy_model_file_path << "\n";
      continue;
    };

    // std::size_t energy_usage = (energy_usage_str);
    // errs() << "inst name: " << instruction_name
    //        << " energy usage: " << energy_usage << "\n";

    config.energy_model[energy_model_name][instruction_name.str()] =
        energy_usage;
  }
}

void ConfigReader::loadConfig(Config &config) {
  std::filesystem::path base_directory{"."};
  auto icconfig_result = std::getenv("IC_CONFIG_DIR");
  if (icconfig_result) {
    base_directory = icconfig_result;
  }
  std::string config_file_path{base_directory / "config.yaml"};

  ErrorOr<std::unique_ptr<MemoryBuffer>> mb =
      MemoryBuffer::getFile(config_file_path);
  if (!mb) {
    errs() << "Error opening file at " << config_file_path << "\n";
    return;
  }

  yaml::Input yin((*mb)->getBuffer());

  yin >> config;
  if (auto error = yin.error()) {
    errs() << error.message() << "\n";
    errs() << "Error reading config file at " << config_file_path << ".\n";
    return;
  }

  for (auto &energy_model_name : config.energy_model_names) {
    loadEnergyModel(config, base_directory, energy_model_name);
  }
  config.loaded = true;
}

ConfigReader::Result ConfigReader::run(Module &M, ModuleAnalysisManager &MAM) {
  Config config;
  loadConfig(config);
  return config;
}

	// Need this function to compute values for loop variables from IC count (n0, n1, ...)
Function *buildSCEVFunction(LLVMContext &Ctx, ScalarEvolution &SE,
                             const SCEV *S, Module *OrigM, Module* NewM) {

  Type *SCEVTy = S->getType();
    SmallVector<Value *, 4> FreeVars;
    SmallPtrSet<Value *, 4> Seen;

    // Walk the SCEV to find all SCEVUnknown leaves
    std::function<void(const SCEV *)> collectVars = [&](const SCEV *Expr) {
        if (const auto *U = dyn_cast<SCEVUnknown>(Expr)) {
            if (Seen.insert(U->getValue()).second)
                FreeVars.push_back(U->getValue());
            return;
        }
        // Recurse into operands
        if (const auto *NAry = dyn_cast<SCEVNAryExpr>(Expr))
            for (const SCEV *Op : NAry->operands()) collectVars(Op);
        else if (const auto *Cast = dyn_cast<SCEVCastExpr>(Expr))
            collectVars(Cast->getOperand());
        else if (const auto *UDiv = dyn_cast<SCEVUDivExpr>(Expr)) {
            collectVars(UDiv->getLHS());
            collectVars(UDiv->getRHS());
        }
    };
    collectVars(S);

    // Build function signature — one i64 param per free variable
  SmallVector<Type *, 4> ParamTypes;
  for (Value *V : FreeVars)
    ParamTypes.push_back(V->getType());

  FunctionType *FT = FunctionType::get(SCEVTy, ParamTypes, false);

    Function *F = Function::Create(FT, Function::ExternalLinkage, "scev_eval", NewM);

    // Name the parameters after the original values
    for (auto [Idx, Arg] : llvm::enumerate(F->args())) {
        Arg.setName(FreeVars[Idx]->getName() + "_arg");
    }

    BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", F);
    IRBuilder<> B(Entry);
    Instruction *Ret = B.CreateRet(UndefValue::get(SCEVTy));

    // Rewrite the SCEV replacing free vars with function arguments
    // Build a map: original Value* -> new Argument*
    ValueToValueMapTy VMap;
    for (auto [Idx, FreeVar] : llvm::enumerate(FreeVars)) {
        VMap[FreeVar] = F->getArg(Idx);
    }

    // Replace SCEVUnknown values using SE.rewriteSCEV or manual substitution
    auto remapSCEV = [&](const SCEV *Expr) -> const SCEV * {
        // Replace each SCEVUnknown with a new SCEVUnknown wrapping the new arg
        struct Rewriter : public SCEVRewriteVisitor<Rewriter> {
            ValueToValueMapTy &VMap;
            Rewriter(ScalarEvolution &SE, ValueToValueMapTy &VMap)
                : SCEVRewriteVisitor(SE), VMap(VMap) {}

            const SCEV *visitUnknown(const SCEVUnknown *Expr) {
                auto It = VMap.find(Expr->getValue());
                if (It != VMap.end())
                    return SE.getUnknown(It->second);
                return Expr;
            }
        };
        Rewriter RW(SE, VMap);
        return RW.visit(Expr);
    };

    const SCEV *RemappedS = remapSCEV(S);

    // Expand the remapped SCEV into the new function
    SCEVExpander Exp(SE, NewM->getDataLayout(), "scev-extract");
    Value *V = Exp.expandCodeFor(RemappedS, SCEVTy, Ret);

    B.SetInsertPoint(Ret);
    Value *Zero = ConstantInt::get(SCEVTy, 0);
    Value *IsNeg = B.CreateICmpSLT(V, Zero, "is_neg");
    Value *Clamped = B.CreateSelect(IsNeg, Zero, V, "clamped");

    Ret->setOperand(0, Clamped);

    errs() << "Extracted SCEV Function\n";
    F->print(outs());

    return F;
}

CounterFunctionAnalysis::Result
CounterFunctionAnalysis::run(Function &F, FunctionAnalysisManager &FAM) {
  CounterFunctionAnalysis::Result result;
  result.function = &F;
  if (F.isDeclaration())
    return result;
  result.fid = Variable::latest_id["f"]++;

  auto &MAMProxy = FAM.getResult<ModuleAnalysisManagerFunctionProxy>(F);
  Module &M = *F.getParent();
  auto &config = *MAMProxy.getCachedResult<ConfigReader>(M);

  if (config.verbose)
    errs() << "Counting function " << demangle(F.getName()) << "\n";

  CounterFunctionAnalysis::BlockToLoops BlTL{};
  CounterFunctionAnalysis::BoundsToLoops BoTL{};
  std::vector<Loop *> unbounded_loops{};

  std::map<std::string, std::string> loop_bound_map{};
  std::map<Loop *, ExprHandle> loop_exprs{};
  std::map<const SCEV*, std::unique_ptr<llvm::Module>> loopNMap{};

  LoopInfo &LI = FAM.getResult<LoopAnalysis>(F);
  ScalarEvolution &SE = FAM.getResult<ScalarEvolutionAnalysis>(F);
  for (Loop *loop : LI) {
  	annotateLoop(loop, SE, F.getContext());
    assignLoopsToBasicBlocks(BlTL, loop);
    assignLoopsToLoopBounds(BoTL, loop_bound_map, loopNMap, unbounded_loops, loop, SE);
  }

  // assign variables to the loops
  createExpressionsForLoops(BoTL, loop_bound_map, loopNMap, unbounded_loops, loop_exprs, config);
  result.loop_bound_map = std::move(loop_bound_map);

  countInstructions(result, loop_exprs, BlTL, config);

  return result;
}

  std::string printValueTree(Value *V, SmallPtrSet<Value*, 8> &visited) {
  if (!visited.insert(V).second)
    return "<cycle>";

  if (llvm::isa<llvm::Argument>(V) || llvm::isa<llvm::Constant>(V)) {
    if (V->hasName()) return V->getName().str();

    std::string result;
    llvm::raw_string_ostream resultStream(result);
    V->print(resultStream);
    return resultStream.str();
  }

  auto *I = dyn_cast<Instruction>(V);
  if (!I)
    return V->hasName() ? V->getName().str() : "<unknown>";

  std::string result = std::string(I->getOpcodeName()) + "(";
  for (unsigned i = 0; i < I->getNumOperands(); ++i) {
    if (i > 0) result += ", ";
    result += printValueTree(I->getOperand(i), visited);
  }
  return result + ")";
}

  // Wrapper
  std::string printValueTree(Value *V) {
  SmallPtrSet<Value*, 8> visited;
  return printValueTree(V, visited);
}

void CounterFunctionAnalysis::createExpressionsForLoops(
    const BoundsToLoops &BoTL, std::map<std::string, std::string> &loop_bound_map,
    std::map<const SCEV*, std::unique_ptr<llvm::Module>> &loopNMap,
    const std::vector<Loop *> &unbounded_loops,
    std::map<Loop *, ExprHandle> &loop_exprs, Config &config) {
  outs() << "Create Expressions For loops\n";
  if (config.verbose)
    outs() << "Assigning vars to the loops:\n";
  for (auto &[bounds, loops] : BoTL) {
    outs() << "  Start: " << *bounds.start << "\n";
    outs() << "  Step: " << *bounds.step << "\n";
    outs() << "  btc: " << *bounds.btc << "\n";
    outs() << "\n";

    size_t nNumber = Variable::latest_id["n"];
    std::string loopBTC;
    llvm::raw_string_ostream rso(loopBTC);

    rso << *bounds.btc;
    rso.flush();   // important

    std::unique_ptr<llvm::Module> loopModule = std::move(loopNMap.at(bounds.btc));
    loopModule->setModuleIdentifier("n"+std::to_string(nNumber));

    std::error_code EC;
    std::string Filename = "scev_n"+std::to_string(nNumber) + ".bc";
    llvm::raw_fd_ostream OS(Filename, EC, llvm::sys::fs::OF_None);
    if (EC) {
      llvm::errs() << "Could not open file: " << EC.message() << "\n";
      return;
    }
    llvm::WriteBitcodeToFile(*loopModule, OS);
    OS.flush();

    loop_bound_map["n"+std::to_string(nNumber)] = loopBTC;
    ExprHandle loop_expr{var(Variable::latest_id["n"]++)};
    for (auto loop : loops) {
      loop_exprs[loop] = loop_expr;
      if (config.verbose)
        outs() << "Added " << loop_exprs[loop] << " to a loop\n";
    }
  }

  for (auto *loop : unbounded_loops) {
    Value *bound = extractBoundFromExitCondition(loop);
    if (bound) {
      size_t nNumber = Variable::latest_id["n"];
      std::string boundValueTree = printValueTree(bound);
      loop_bound_map["n"+std::to_string(nNumber)] = boundValueTree;
      outs() << "  -> bound expr: " << printValueTree(bound) << "\n";
    }

    ExprHandle loop_expr{var(Variable::latest_id["n"]++)};
    loop_exprs[loop] = std::move(loop_expr);
    if (config.verbose)
      errs() << "Added " << loop_exprs[loop] << " to a loop\n";
  }
}
  void CounterFunctionAnalysis::assignLoopsToLoopBounds(
      BoundsToLoops &BTL, std::map<std::string, std::string> &loop_bound_map,
      std::map<const SCEV*, std::unique_ptr<llvm::Module>> &loopNMap,
      std::vector<Loop *> &unbounded_loops, Loop *loop,
      ScalarEvolution &SE) {
  for (Loop *l_inner : *loop) {
    assignLoopsToLoopBounds(BTL, loop_bound_map, loopNMap, unbounded_loops, l_inner, SE);
  }

  BasicBlock *header = loop->getHeader();
  Function *F = header->getParent();
  outs() << "[Loop in " << F->getName() << "] "
         << "Header: " << header->getName() << "\n";

  // Print the exit condition so we can see what LLVM sees
  BasicBlock *exitingBB = loop->getExitingBlock();
  if (!exitingBB) {
    outs() << "  -> multiple exiting blocks (or none), getBounds() will fail\n";
  } else {
    outs() << "  -> exiting block: " << exitingBB->getName() << "\n";
    if (auto *BI = dyn_cast<BranchInst>(exitingBB->getTerminator())) {
      if (BI->isConditional())
        outs() << "  -> exit condition: " << *BI->getCondition() << "\n";
    }
  }

  const SCEV *Start = nullptr;
  const SCEV *Step  = nullptr;
  const SCEV *BTC   = nullptr;

  // First try LLVM's canonical induction variable.
  if (PHINode *IV = loop->getInductionVariable(SE)) {

      outs() << "  -> induction variable: " << *IV << "\n";

      if (const auto *AR =
              dyn_cast<SCEVAddRecExpr>(SE.getSCEV(IV))) {

          Start = AR->getStart();
          Step  = AR->getStepRecurrence(SE);
          BTC   = SE.getBackedgeTakenCount(loop);
      }
  }

  // ------------------------------------------------------------
  // Fallback: search all PHIs in the loop header.
  // ------------------------------------------------------------
  if (!Start) {

      for (PHINode &Phi : loop->getHeader()->phis()) {

          const SCEV *S =  nullptr;
          if (SE.isSCEVable(Phi.getType())) {
            outs() << "is scevable\n";
            S = SE.getSCEV(&Phi);
          } else {
            outs() << "is not scevable\n";
            continue;
          }


          if (const auto *AR =
                  dyn_cast<SCEVAddRecExpr>(S)) {

              outs() << "  -> non-canonical IV: " << Phi << "\n";

              Start = AR->getStart();
              Step  = AR->getStepRecurrence(SE);
              BTC   = SE.getBackedgeTakenCount(loop);

              break;
          } else if (const auto *AR = dyn_cast<SCEVMulExpr>(S)) {
            outs() << "   -> scev mul expr\n";
              BTC = SE.getBackedgeTakenCount(loop);
          } else {


              outs() << "  -> non AddRec SCEV type: " << *S << "\n";

              switch (S->getSCEVType()) {
                case scConstant:
                  outs() << "     kind: constant\n";
                  break;

                case scTruncate:
                  outs() << "     kind: truncate\n";
                  break;

                case scZeroExtend:
                  outs() << "     kind: zero extend\n";
                  break;

                case scSignExtend:
                  outs() << "     kind: sign extend\n";
                  break;

                case scAddExpr:
                  outs() << "     kind: add expression\n";
                  break;

                case scMulExpr:
                  outs() << "     kind: multiply expression\n";
                  break;

                case scUDivExpr:
                  outs() << "     kind: unsigned division\n";
                  break;

                case scSMaxExpr:
                case scUMaxExpr:
                  outs() << "     kind: max expression\n";
                  break;

                case scUnknown:
                  outs() << "     kind: unknown (SCEVUnknown)\n";
                  break;

                default:
                  outs() << "     kind: other\n";
                  break;
              }

              BTC = SE.getBackedgeTakenCount(loop);
          }

      }
  }

  // ------------------------------------------------------------
  // Give up if we still have no induction variable.
  // ------------------------------------------------------------
  if (!Start || !Step || !BTC) {
    outs() << "   -> !Start || !Step || !BTC\n";
      unbounded_loops.push_back(loop);
      return;
  }

  // ------------------------------------------------------------
  // Check whether SCEV could compute trip count.
  // ------------------------------------------------------------
  if (isa<SCEVCouldNotCompute>(BTC)) {
    outs() << "   -> isa<SCEVCouldNotCompute>(BTC)\n";
      unbounded_loops.push_back(loop);
      return;
  }


  Type *IVTy  = Step->getType();
  Type *BTCTy = BTC->getType();

  // Decide which is wider
  Type *WideTy = (BTCTy->getIntegerBitWidth() > IVTy->getIntegerBitWidth())
               ? BTCTy : IVTy;
  const SCEV *WideStart = SE.getNoopOrSignExtend(Start, WideTy);
  const SCEV *WideStep  = SE.getNoopOrSignExtend(Step,  WideTy);
  const SCEV *WideBTC   = SE.getNoopOrZeroExtend(BTC,   WideTy);

  // ------------------------------------------------------------
  // Construct "last value" of the IV.
  // final = Start + Step * BTC
  // ------------------------------------------------------------
  const SCEV *Final =
      SE.getAddExpr(
          WideStart,
          SE.getMulExpr(WideStep, WideBTC));

  llvm::LLVMContext& Ctx = header->getContext();
  auto NewM = std::make_unique<Module>("scev_module", Ctx);

  llvm::InitializeNativeTarget();
  auto Triple = llvm::sys::getDefaultTargetTriple();
  NewM->setTargetTriple(Triple);

  std::string Error;
  const llvm::Target *Target = llvm::TargetRegistry::lookupTarget(Triple, Error);
  if (!Target) {
    outs() << "Failed to lookup target: " << Error << "\n";
    return;
  }


  std::unique_ptr<llvm::TargetMachine> TM(
    Target->createTargetMachine(Triple, "generic", "", {}, std::nullopt)
  );
  NewM->setDataLayout(TM->createDataLayout());

  Function *scevF = buildSCEVFunction(Ctx, SE, BTC, header->getModule(), NewM.get());

  // Add main calling scev_eval with n=100
  //addMainWrapper(*NewM, scevF);

  errs() << "=== Full Module ===\n";
  NewM->print(errs(), nullptr);

  outs() << "  Start = " << *Start << "\n";
  outs() << "  Step  = " << *Step  << "\n";
  outs() << "  BTC   = " << *BTC   << "\n";
  outs() << "  Final = " << *Final << "\n";
  //outs() << " Expanded btc: " << *newF << "\n";

  outs() << "Loop: " << loop->getLoopID() << "\n";

  loopNMap[BTC] = std::move(NewM);

  // ------------------------------------------------------------
  // If your LoopBoundKey stores SCEVs:
  // ------------------------------------------------------------
  LoopBoundKeySCEV Key{
      const_cast<SCEV *>(Start),
      const_cast<SCEV *>(Step),
      const_cast<SCEV *>(BTC)
  };

  BTL[Key].push_back(loop);
}

// Helper: get the loop-invariant operand from the exit icmp
Value *CounterFunctionAnalysis::extractBoundFromExitCondition(Loop *loop) {

  PHINode *Phi = loop->getCanonicalInductionVariable();
  if (!Phi) return nullptr;

  Value *Initial = nullptr;
  for (unsigned i = 0; i < Phi->getNumIncomingValues(); ++i) {
    BasicBlock *Pred = Phi->getIncomingBlock(i);
    if (!loop->contains(Pred)) {
      Initial = Phi->getIncomingValue(i);
      break;
    }
  }


  BasicBlock *exitingBB = loop->getExitingBlock();
  if (!exitingBB) return nullptr;

  auto *BI = dyn_cast<BranchInst>(exitingBB->getTerminator());
  if (!BI || !BI->isConditional()) return nullptr;

  auto *cmp = dyn_cast<ICmpInst>(BI->getCondition());
  if (!cmp) return nullptr;

  // Return the operand that is NOT defined inside the loop
  for (Value *op : cmp->operands())
    if (auto *I = dyn_cast<Instruction>(op)) {
      if (!loop->contains(I->getParent()))
        return op; // loop-invariant: this is the bound
    } else if (isa<Argument>(op)) {
      return op; // direct function argument
    }

  return nullptr;
}

bool isLoopControlInstruction(Instruction *I, Loop *L,
																			ScalarEvolution &SE) {
	if (SE.isSCEVable(I->getType())) {
		if (auto *AR = dyn_cast<SCEVAddRecExpr>(SE.getSCEV(I)))
			if (AR->getLoop() == L)
				return true;
	}

	if (auto *Cmp = dyn_cast<ICmpInst>(I)) {
		BasicBlock *BB = Cmp->getParent();
		if (L->isLoopExiting(BB)) {
			const SCEV *BTC = SE.getExitCount(L, BB);
			if (!isa<SCEVCouldNotCompute>(BTC))
				return true;
		}
	}

	if (I->isTerminator() && L->isLoopExiting(I->getParent()))
		return true;

	return false;
}

	// Not safe to be ignored by dbg.pass.no.op, dbg information is tampered
	//TODO: move this block into dbg pass and not here, would be much cleanr...
void CounterFunctionAnalysis::annotateLoop(Loop *L, ScalarEvolution &SE, LLVMContext &Ctx) {
	for (BasicBlock *BB : L->blocks()) {
		for (Instruction &I : *BB) {
			if (isLoopControlInstruction(&I, L, SE))
				I.setMetadata("ps.loop", MDNode::get(Ctx, {}));
		}
	}

	for (Loop *SubL : L->getSubLoops())
		annotateLoop(SubL, SE, Ctx);
}

void CounterFunctionAnalysis::assignLoopsToBasicBlocks(BlockToLoops &BTL,
                                                       Loop *loop) {
  for (Loop *l_inner : *loop) {
    assignLoopsToBasicBlocks(BTL, l_inner);
  }
  for (BasicBlock *BB : loop->getBlocks()) {
    BTL[BB].push_back(loop);
  }
}

void CounterFunctionAnalysis::countInstructions(
    Result &result, std::map<Loop *, ExprHandle> &loop_exprs,
    BlockToLoops &BlTL, Config &config) {

  for (auto &BB : *result.function) {
    for (auto &inst : BB) {
      std::string opcode_name = std::string{inst.getOpcodeName()};

      bool isSharedLoad = inst.getMetadata("epi.shared_load") != nullptr;
      bool isSharedStore = inst.getMetadata("epi.shared_store") != nullptr;
      if (isSharedLoad) {
        opcode_name = "shared_load";
      } else if (isSharedStore) {
        opcode_name = "shared_store";
      }

      llvm::Type* type = inst.getType();

      if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(&inst)) {
        if (ret->getNumOperands() == 0) {
          // ret void
          type = llvm::Type::getVoidTy(inst.getContext());
        }
      }

      if (auto *br = llvm::dyn_cast<llvm::BranchInst>(&inst)) {
        if (br->isUnconditional()) {
          // br label
          type = llvm::Type::getVoidTy(inst.getContext());
        }
      }

      if (!llvm::isa<llvm::LoadInst>(&inst) && inst.getNumOperands() > 0) {
	      type = inst.getOperand(0)->getType();
      }

    	std::string typeStr;
    	llvm::raw_string_ostream rso(typeStr);
    	type->print(rso);

      auto it = std::find(config.instructions_to_count.begin(),
                    config.instructions_to_count.end(), opcode_name);

      opcode_name = opcode_name + "*" + typeStr;

      llvm::outs() << "Processing Instruction: " << opcode_name << "\n";
    	bool isDbgMarked = (inst.getMetadata("dbg.pass.no.op") != nullptr);
    	bool isLoopMarked = inst.getMetadata("ps.loop") != nullptr;
      bool dontCount = (it == config.instructions_to_count.end()) || (isDbgMarked && !isLoopMarked);
      bool isCallOrInvoke = isa<CallInst, InvokeInst>(inst);
      if (dontCount && !isCallOrInvoke) {
        continue;
      }

      ExprHandle expr = constant(1);
      if (BlTL.count(&BB)) {
        for (auto loop : BlTL[&BB]) {
          expr = mul({loop_exprs[loop], expr});
        }
      }
      if (!dontCount) {
        if (result.instruction_costs.count(opcode_name)) {
          result.instruction_costs[opcode_name] =
              add({result.instruction_costs[opcode_name], expr});

        } else {
          result.instruction_costs[opcode_name] = expr;
        }
      }

      if (isCallOrInvoke) {
        Function *called_F = nullptr;
        if (isa<CallInst>(inst)) {
          called_F = cast<CallInst>(inst).getCalledFunction();
        } else if (isa<InvokeInst>(inst)) {
          called_F = cast<InvokeInst>(inst).getCalledFunction();
        }

        if (!called_F)
          continue;                        // in case it's null
        if (called_F == result.function) { // if it's a recursion
          result.recursion_expr =
              add({result.recursion_expr,
                   mul({expr, var(Variable::latest_id["r"]++)})});
          continue;
        }

      	outs() << "Checking call to " << called_F->getName().str() << "\n";
      	std::string fNameToAdd = "";
        auto it =
            std::find(config.instructions_to_count.begin(),
                      config.instructions_to_count.end(), called_F->getName());
      	if (it != config.instructions_to_count.end()) {
      		fNameToAdd = called_F->getName().str();
      	} else {
      		auto demangled = llvm::demangle(called_F->getName().str());
      		auto demangled_fname = StringRef(demangled).split('(').first;
      		outs() << "Checking call to " << demangled_fname << "\n";
      		it = std::find(config.instructions_to_count.begin(),
					config.instructions_to_count.end(), demangled_fname);
      		if (it != config.instructions_to_count.end()) {
      			fNameToAdd = demangled_fname;
      		}
      	}
        if (it != config.instructions_to_count
                      .end()) { // if it should be counted as an instruction
          if (config.verbose)
            errs() << "user specified call.\n";
          std::string fname = fNameToAdd + "*";
          fname += typeStr;
        	outs() << "adding to instruction cots: " << fname << "\n";
          if (result.instruction_costs.count(fname)) {
            result.instruction_costs[fname] =
                add({result.instruction_costs[fname], expr});

          } else {
            result.instruction_costs[fname] = expr;
          }
          continue;
        }
        if (called_F->isDeclaration()) {
          continue; // if it's a declaration, skip
        }

        if (result.outgoing_calls_costs.count(called_F)) {
          result.outgoing_calls_costs[called_F] =
              add({result.outgoing_calls_costs[called_F], expr});
        } else {
          result.outgoing_calls_costs[called_F] = expr;
        }
      }
    }
  }
}

CountAggregationFunctionAnalysis::Result
CountAggregationFunctionAnalysis::getICFunctionAnalysisResult(
    Function *F, FunctionAnalysisManager &FAM) {
  CounterFunctionAnalysis::Result *result_ptr =
      FAM.getCachedResult<CounterFunctionAnalysis>(*F);
  if (!result_ptr) {
    errs() << "There was no cached result for Function: " << F->getName()
           << "!\n";
    return CounterFunctionAnalysis::Result();
  }
  return *result_ptr;
}

void CountAggregationFunctionAnalysis::doAggregation(Result &prev_result,
                                                     Result &called_result,
                                                     ExprHandle call_expr,
                                                     Config &config) {
  for (auto &[k, v] : called_result.instruction_costs) {
    if (Constant *c = std::get_if<Constant>(v.get())) {
      if (c->value == 0) {
        continue;
      }
    }
    ExprHandle expr_base =
        mul({substituteRecursionVariables(called_result.recursion_expr),
             call_expr});
    ExprHandle expr_actual = v;
    if (config.aggregate_level == Config::FID) {
      expr_actual = var(called_result.fid, 1, 1, "f");
    } else if (config.aggregate_level == Config::Constants) {
      if (Constant *c = std::get_if<Constant>(v.get())) {
        expr_actual = std::make_shared<Expr>(*c);
      } else {
        expr_actual = var(called_result.fid, 1, 1, "f");
      }
    }

    ExprHandle expr = mul({expr_base, expr_actual});

    if (prev_result.instruction_costs.count(k)) {
      prev_result.instruction_costs[k] =
          add({prev_result.instruction_costs[k], expr});
    } else {
      prev_result.instruction_costs[k] = expr;
    }
  }
}

CountAggregationFunctionAnalysis::Result
CountAggregationFunctionAnalysis::run(Function &F,
                                      FunctionAnalysisManager &FAM) {
  auto &MAMProxy = FAM.getResult<ModuleAnalysisManagerFunctionProxy>(F);
  Module &M = *F.getParent();
  auto &config = *MAMProxy.getCachedResult<ConfigReader>(M);

  if (config.verbose)
    errs() << "In ICAggregationFunctionAnalysis for " << F.getName() << ":\n";
  Result prev_result = getICFunctionAnalysisResult(&F, FAM);

  for (auto &[called_F, call_expr] : prev_result.outgoing_calls_costs) {
    if (config.verbose)
      errs() << "For call to function " << called_F->getName() << ":\n";
    if (config.verbose)
      errs() << "Getting result of " << called_F->getName() << "\n";
    Result called_F_result =
        FAM.getResult<CountAggregationFunctionAnalysis>(*called_F);
    if (config.verbose)
      errs() << "Got result of " << called_F->getName() << "\n";
    doAggregation(prev_result, called_F_result, call_expr, config);
  }

  return prev_result;
}

CounterModuleAnalysis::Result
CounterModuleAnalysis::run(Module &M, ModuleAnalysisManager &MAM) {
  auto &config = MAM.getResult<ConfigReader>(M);

  std::vector<Function *> functions_sorted;
  for (auto &F : M) {
    functions_sorted.push_back(&F);
  }
  llvm::sort(functions_sorted.begin(), functions_sorted.end(),
             FunctionPointerComparator());

  Result result;
  FunctionAnalysisManager &FAM =
      MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();

  for (auto F : functions_sorted) {
    if (config.verbose) {
      errs() << "Running ICFunctionAnalysis for " << F->getName() << "\n";
    }
    result.function_results[F] = FAM.getResult<CounterFunctionAnalysis>(*F);
  }

  for (auto F : functions_sorted) {
    if (config.verbose) {
      errs() << "Running ICAggregationFunctionAnalysis for " << F->getName()
             << "\n";
    }
    result.function_results[F] =
        FAM.getResult<CountAggregationFunctionAnalysis>(*F);
  }

	std::filesystem::path outputDir = "ic_out";
	std::filesystem::create_directories(outputDir);

	std::string moduleName = M.getSourceFileName();
	std::error_code EC;
	std::string outputFile = outputDir / (moduleName + ".ll");
	raw_fd_ostream OS(outputFile, EC, sys::fs::OF_None);
	if (EC) {
		errs() << "Could not open file: " << EC.message() << "\n";
	}
	M.print(OS, nullptr);

  return result;
}
} // namespace IC

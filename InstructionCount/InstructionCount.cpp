#include "DebugToggle.hpp"
#include "Expression.hpp"
#include "ICAnalyses.hpp"
#include "ProtoTransform.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/Analysis.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Mangler.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Pass.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/PassPlugin.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/YAMLTraits.h>
#include <llvm/Support/raw_ostream.h>
#include <map>
#include <string>
#include <vector>
#include <google/protobuf/util/json_util.h>

#include "llvm/IR/Module.h"

#include "interface.pb.h"

using namespace llvm;

namespace IC {

struct InstructionCount : PassInfoMixin<InstructionCount> {

  bool run_test(std::string test_name, std::string actual,
                std::string expected) {
    if (actual != expected) {
      errs() << test_name << " failed, expected: " << expected
             << "\n\tactual: " << actual << "\n";
      return false;
    }
    errs() << test_name << " passed\n";
    return true;
  }
  void tests() {
    // expressions

    // 0 constant
    {
      ExprHandle c0 = constant(0);
      std::string expected = "0";
      std::string actual = toString(c0);
      run_test("0 constant", actual, expected);
    }

    {
      ExprHandle c0 = constant(0);
      ExprHandle c1 = constant(1);
      std::string expected = "1";
      std::string actual = toString(add({c0, c1}));
      run_test("0 plus 1", actual, expected);
    }

    {
      ExprHandle v0 = var(0);
      ExprHandle c1 = constant(1);
      ExprHandle a1 = add({constant(1), v0});
      ExprHandle c2 = constant(2);
      ExprHandle a2 = add({a1, c2});
      run_test("nested addition", toString(a2), "3+n0");
      run_test("c1 unchanged", toString(c1), "1");
      run_test("a1 unchanged", toString(a1), "1+n0");
    }

    {
      ExprHandle m0 = mul({var(0), add({constant(1), var(1)})});
      run_test("distributive law and mult with 1", toString(m0), "n0+(n0*n1)");
      ExprHandle c21 = constant(21);
      ExprHandle c2 = constant(2);
      ExprHandle m1 = mul({m0, c21, c2});
      run_test("distributive, nested mult and constant propagation",
               toString(m1), "42n0+(42n0*n1)");
      run_test("mul 0", toString(mul({m1, constant(0)})), "0");
      run_test("m1 unchanged", toString(m1), "42n0+(42n0*n1)");
    }

    {
      ExprHandle n0 = var(0);
      ExprHandle n0p2 = var(0, 3, 2);
      ExprHandle m0 = mul({n0p2, n0});
      run_test("multiplying same variable", toString(m0), "3n0^3");
    }
    {
      Variable::latest_id = {};
      ExprHandle r0 = var(Variable::latest_id["r"]++, 1, 1, "r");
      ExprHandle n0 = var(Variable::latest_id["n"]++);

      ExprHandle m0 = mul({r0, n0});
      ExprHandle a0 = add({m0, n0});
      ExprHandle t0 = substituteRecursionVariables(r0);
      run_test("original variable unchanged after substitute 1", toString(r0),
               "r0");
      run_test("substituted variable", toString(t0), "n1");
      ExprHandle t1 = substituteRecursionVariables(m0);
      run_test("original variable unchanged after substitute 2", toString(r0),
               "r0");
      run_test("original multiplication unchanged after substitute",
               toString(m0), "r0*n0");
      run_test("substituted variable in multiplication", toString(t1), "n2*n0");
      ExprHandle t2 = substituteRecursionVariables(a0);
      run_test("original variable unchanged after substitute 3", toString(r0),
               "r0");
      run_test("original addition unchanged after substitute", toString(a0),
               "(r0*n0)+n0");
      run_test("substituted variable in addition", toString(t2), "(n3*n0)+n0");
    }
  }

	bool outputToJson(Module &M, const CounterModuleAnalysis::Result &MR,
	Config &config, const std::string &energy_model_name) {
	  energy_estimation::Report report;
  	std::cout << "report" << std::endl;


  	llvm::SmallVector<std::string> headerInsts{};

  	for (auto &inst : config.instructions_to_count) {
  		for (const std::string &prefix : {inst + "*", inst + "_uniform*"}) {
  			if (EMDebugEnabled()) outs() << "Checking instruction " << prefix << "\n";

  			for (auto &[F, FR] : MR.function_results) {
  				auto name = F->getName();
  				if (EMDebugEnabled()) outs() << "Checking function : " << name << " against prefix: " + prefix + "\n";

  				for (auto it = FR.instruction_costs.lower_bound(prefix); it != FR.instruction_costs.end(); ++it) {
  					if (it->first.rfind(prefix, 0) != 0) break;

  					std::string instKey = it->first;

  					if (!llvm::is_contained(headerInsts, instKey)) {
  						headerInsts.push_back(instKey);
  						if (EMDebugEnabled()) outs() << "Add Instruction to output: " << prefix << " - " << instKey << "\n";
  					}
  				}
  			}
  		}
  	}

  	auto *functions = report.mutable_functions();
  	for (auto &[function, FR] : MR.function_results) {
  		if (function->isDeclaration())
  			continue;

  		for (auto &[key, bound] : FR.loop_bound_map) {
  			report.mutable_variables()->insert({key, bound});
  		}

  		energy_estimation::Function FunctionInfo;
  		FunctionInfo.set_name(function->getName().str());
  		FunctionInfo.set_demangled(demangle(function->getName()));

  		// (type, instruction, isUniform)
  		using MergeKey = std::tuple<int, int, bool>;
  		std::vector<MergeKey> merged_order;
  		std::map<MergeKey, ExprHandle> merged_costs;

  		for (auto &inst : headerInsts) {
  			size_t pos = inst.find('*');

  			if (pos == std::string::npos) {
  				continue;
  			}

  			if (FR.instruction_costs.find(inst) == FR.instruction_costs.end()) {
  				continue;
  			}

  			std::string key1 = inst.substr(0, pos);
  			std::string key2 = inst.substr(pos + 1);

  			energy_estimation::ValueType type = ProtoTransform::typeToProto(key2, key1);
  			bool termIsUniform = false;
  			energy_estimation::Instruction proto_inst = ProtoTransform::instToProto(key1, termIsUniform);
  			llvm::outs() << ValueType_Name(type).c_str() << "\n";

  			size_t energy_model = config.energy_model[energy_model_name][inst];
  			if (!energy_model) energy_model = 1;
  			if (EMDebugEnabled()) outs() << "Energy Model: " << energy_model << "\n";
  			ExprHandle expr = mul({FR.instruction_costs.at(inst),
										constant(energy_model)});

  			auto mergeKey = std::make_tuple(static_cast<int>(type), static_cast<int>(proto_inst), termIsUniform);
  			auto it = merged_costs.find(mergeKey);
  			if (it == merged_costs.end()) {
  				merged_costs[mergeKey] = expr;
  				merged_order.push_back(mergeKey);
  			} else {
  				it->second = add({it->second, expr});
  			}
  		}

  		for (auto &mergeKey : merged_order) {
  			energy_estimation::InstructionCount* entry = FunctionInfo.add_count();
  			entry->set_type(static_cast<energy_estimation::ValueType>(std::get<0>(mergeKey)));
  			entry->set_instruction(static_cast<energy_estimation::Instruction>(std::get<1>(mergeKey)));
  			entry->set_expression(toString(merged_costs[mergeKey]));
  			entry->set_is_uniform(std::get<2>(mergeKey));
  		}

  		(*functions)[FR.fid]  = FunctionInfo;
  	}

  	std::string json_output;
  	google::protobuf::util::JsonPrintOptions options;
  	options.add_whitespace = true;
  	options.always_print_primitive_fields = true;
  	options.preserve_proto_field_names = true;

  	auto status = google::protobuf::util::MessageToJsonString(report, &json_output, options);
  	if (!status.ok()) {
  		std::cerr << "Failed to convert to JSON: " << status.ToString() << std::endl;
  		return 1;
  	}

  	std::filesystem::path file_path("./output");

  	std::string output_filename;
  	raw_string_ostream ofn(output_filename);
  	std::filesystem::path source_file_path = M.getSourceFileName();
  	ofn << source_file_path.filename() << "-" << M.getTargetTriple()
				<< "-" << energy_model_name << ".json";

  	auto icconfigdir_result = std::getenv("IC_OUTPUT_DIR");
  	if (icconfigdir_result) {
  		file_path = icconfigdir_result;
  	}
  	if (!std::filesystem::exists(file_path)) {
  		if (!std::filesystem::create_directories(file_path)) {
  			errs() << "Could not create parent directories of path " << file_path
							 << "\n";
  			return false;
  		};
  	}
  	file_path /= output_filename;

  	std::ofstream out_file(file_path);
  	if (!out_file) {
  		std::cerr << "Failed to open output file" << std::endl;
  		return 1;
  	}
  	out_file << json_output;
  	out_file.close();
  	return true;
  }

  bool outputToCsv(Module &M, const CounterModuleAnalysis::Result &MR,

  Config &config, const std::string &energy_model_name) {
    std::string output_str{};
    raw_string_ostream ostream{output_str};
    std::string var_str{};
    raw_string_ostream ostream_var{var_str};

    llvm::outs() << "Output to csv2\n";

    if (EMDebugEnabled()) {
      for (auto &[F, FR] : MR.function_results) {
        if (F->getName() == "foo") {
          for (auto &[key, value] : FR.instruction_costs) {
            llvm::outs() << key << "\n";
          }
        }
      }
    }

    llvm::SmallVector<std::string> headerInsts{};

    ostream << "Function Name,Demangled Name,fid,total";
    for (auto &inst : config.instructions_to_count) {
      std::string prefix = inst + "*";
      if (EMDebugEnabled()) outs() << "Checking instruction " << prefix << "\n";

      for (auto &[F, FR] : MR.function_results) {
        auto name = F->getName();
        if (EMDebugEnabled()) outs() << "Checking function : " << name << " against prefix: " + prefix + "\n";

        for (auto it = FR.instruction_costs.lower_bound(prefix); it != FR.instruction_costs.end(); ++it) {
          if (it->first.rfind(prefix, 0) != 0) break; // stop when prefix no longer matches
          // it->first and it->second are your key/value

          std::string instKey = it->first;

          if (!llvm::is_contained(headerInsts, instKey)) {
            headerInsts.push_back(instKey);
            if (EMDebugEnabled()) outs() << "Add Instruction to output: " << prefix << " - " << instKey << "\n";

            ostream << "," << instKey;
          }
        }
      }
    }
    ostream << "\n";

    std::map<llvm::Function *, ExprHandle> total_costs;
    for (auto &[F, _] : MR.function_results) {
      total_costs[F] = constant(0);
    }
    for (auto &[F, FR] : MR.function_results) {
      auto name = F->getName();
      if (EMDebugEnabled()) outs() << "Checking function : " << name << "\n";
      for (auto &[key, cost] : FR.instruction_costs) {
        if (EMDebugEnabled()) outs() << "Adding to total costs (" << key << "): " << toString(cost) << "\n";
        total_costs[F] = add({total_costs[F], cost});
      }
    }

    for (auto &[function, FR] : MR.function_results) {
      if (function->isDeclaration())
        continue;

      outs() << "Adding values for function: " << function->getName() << "\n";

      // Name
      const auto name = function->getName();
      ostream << name;
      ostream << ",\"" << demangle(name) << "\"";
      ostream << ",f" << FR.fid;
      ostream << "," << total_costs[function];

      for (auto &inst : headerInsts) {
        size_t pos = inst.find('*');

        if (pos == std::string::npos) {
          // no '*'
          continue;
        }

        std::string key1 = inst.substr(0, pos);
        std::string key2 = inst.substr(pos + 1);

        ExprHandle expr;
        if (FR.instruction_costs.find(inst) == FR.instruction_costs.end()) {
          expr = constant(0);
        } else {
          size_t energy_model = config.energy_model[energy_model_name][inst];
          if (!energy_model) energy_model = 1;
          outs() << "Energy Model: " << energy_model << "\n";
          expr = mul({FR.instruction_costs.at(inst),
                      constant(energy_model)});
        }
        ostream << "," << expr;
      }
      ostream << "\n";


      for (auto &[key, bound] : FR.loop_bound_map) {
        ostream_var << key << "=" << bound << "\n";
      }
    }

    std::filesystem::path file_path("./output");

    std::string output_filename;
    raw_string_ostream ofn(output_filename);
    std::filesystem::path source_file_path = M.getSourceFileName();
    ofn << source_file_path.filename() << "-" << M.getTargetTriple()
        << "-" << energy_model_name << ".csv";

    auto icconfigdir_result = std::getenv("IC_OUTPUT_DIR");
    if (icconfigdir_result) {
      file_path = icconfigdir_result;
    }
    if (!std::filesystem::exists(file_path)) {
      if (!std::filesystem::create_directories(file_path)) {
        errs() << "Could not create parent directories of path " << file_path
               << "\n";
        return false;
      };
    }
    file_path /= output_filename;

    std::ofstream csv_file{};
    csv_file.open(file_path);
    if (!csv_file.is_open()) {
      errs() << "Error while trying to open output file at " << file_path
             << "\n";
      return false;
    }

    // std::string str;
    // raw_string_ostream stros{str};
    // stros << M;
    //
    // csv_file << str << "\n";

    csv_file << output_str;
    csv_file << var_str;
    csv_file.close();
    return true;
  }

  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM) {
    // read config
    Config &config = MAM.getResult<ConfigReader>(M);
    if (!config.loaded) {
      errs() << "Exiting Pass Early\n";
      return PreservedAnalyses::all();
    }

    // run tests
    if (config.run_tests) {
      tests();
      exit(0);
    }

    llvm::Triple triple(M.getTargetTriple());
    if (!config.isTargetValid(triple)) {
      if (config.verbose)
        errs() << "Skipping non-device module\n";
      return PreservedAnalyses::all();
    }
    if (config.verbose) {
      errs() << "Analysing a Module with Target Triple: " << triple.str()
             << "\n";
    }

    if (config.verbose)
      errs() << "Running analysis for Module " << M.getName() << "\n";

    // for (auto &F : M) {
    //   function_variable_ids[&F] = Variable::latest_id["f"]++;
    // }

    auto MR = MAM.getResult<CounterModuleAnalysis>(M);

    for (auto &[energy_model_name, _] : config.energy_model)
      if (!outputToJson(M, MR, config, energy_model_name)) {
        errs() << "Exiting Pass Early\n";
        return PreservedAnalyses::all();
      }

    return PreservedAnalyses::all();
  }

  // static bool isRequired() { return true; }
};

void registerPassBuilderCallbacks(llvm::PassBuilder &PB) {
  PB.registerAnalysisRegistrationCallback(
      [](llvm::FunctionAnalysisManager &FAM) {
        FAM.registerPass([&] { return CounterFunctionAnalysis(); });
        FAM.registerPass([&] { return CountAggregationFunctionAnalysis(); });
      });
  PB.registerAnalysisRegistrationCallback([](llvm::ModuleAnalysisManager &MAM) {
    MAM.registerPass([&] { return CounterModuleAnalysis(); });
    MAM.registerPass([&] { return ConfigReader(); });
  });
  PB.registerPipelineParsingCallback(
      [](llvm::StringRef Name, llvm::ModulePassManager &MPM,
         ArrayRef<llvm::PassBuilder::PipelineElement>) {
        if (Name == "instruction-count") {
          MPM.addPass(InstructionCount());
          return true;
        }
        return false;
      });
  PB.registerOptimizerLastEPCallback([](llvm::ModulePassManager &MPM,
                                        llvm::OptimizationLevel Level) {
      MPM.addPass(InstructionCount());
  });
}

} // namespace IC

/* New PM Registration */
llvm::PassPluginLibraryInfo getInstructionCountPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "InstructionCount", LLVM_VERSION_STRING,
          IC::registerPassBuilderCallbacks};
}

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getInstructionCountPluginInfo();
}

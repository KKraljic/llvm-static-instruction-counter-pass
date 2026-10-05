//
// Created by nick on 7/5/26.
//

#include "DebugToggle.hpp"
#include "ProtoTransform.hpp"

#include <llvm/IR/Instructions.h>

#include <utility>

const llvm::StringMap<ValueType> ProtoTransform::InstTypeMap {
	{"__hadd",  TYPE_FP16},
	{"__hsub",  TYPE_FP16},
	{"__hmul",  TYPE_FP16},
	{"hlog", TYPE_FP16},
	{"hlog2", TYPE_FP16},
	{"hlog10", TYPE_FP16},
	{"hexp", TYPE_FP16},
	{"hexp2", TYPE_FP16},
	{"hexp10", TYPE_FP16},
	{"htanh", TYPE_FP16},
	{"hsqrt", TYPE_FP16},
	// br benchmark only registered UINT32
	{"br",      TYPE_UINT32},
};

std::string capitalize(std::string s) {
	if (!s.empty())
		s[0] = std::toupper(static_cast<unsigned char>(s[0]));
	return s;
}

ValueType ProtoTransform::typeToProto(const std::string &type, const std::string &inst) {
	if (ProtoTransform::InstTypeMap.contains(inst)) {
		return ProtoTransform::InstTypeMap.at(inst);
	}

	if (EMDebugEnabled()) std::cout << "typeToProto: " << type << std::endl;
	
	std::string elemType = type;
	if (elemType.size() >= 2 && elemType.front() == '<' && elemType.back() == '>') {
		std::string inner = elemType.substr(1, elemType.size() - 2);
		size_t lastX = inner.rfind(" x ");
		if (lastX != std::string::npos) {
			elemType = inner.substr(lastX + 3);
		}
	}

  // LLVM integers carry no sign; fptosi results and srem (benchmark_mod on signed types) are
  // measured under the signed types
  if (llvm::StringRef(inst).starts_with("fptosi") || llvm::StringRef(inst).starts_with("srem")) {
    if (elemType == "i8") return TYPE_INT8;
    if (elemType == "i16") return TYPE_INT16;
    if (elemType == "i32") return TYPE_INT32;
    if (elemType == "i64") return TYPE_INT64;
  }

  if (elemType == "float") return TYPE_FP32;
  if (elemType == "double") return TYPE_FP64;
  if (elemType == "half") return TYPE_FP16;
  if (elemType == "i32") return TYPE_UINT32;
  if (elemType == "i64") return TYPE_UINT64;
  if (elemType == "i16") return TYPE_UINT16;
  if (elemType == "i8") return TYPE_UINT8;
  if (elemType == "void") return TYPE_VOID;
	if (elemType.find("ptr") != std::string::npos) {
		return TYPE_PTR;
	}
  return TYPE_ERR;
}

ValueType ProtoTransform::typeToProto(llvm::Type* type) {
  if (!type) return energy_estimation::TYPE_VOID;

  if (type->isFloatTy())
    return energy_estimation::TYPE_FP32;
  if (type->isHalfTy())
    return energy_estimation::TYPE_FP16;
  if (type->isDoubleTy())
    return energy_estimation::TYPE_FP64;
  if (type->isIntegerTy(1))
    return energy_estimation::TYPE_BOOL;
  if (type->isIntegerTy(8))
    return energy_estimation::TYPE_UINT8;
  if (type->isIntegerTy(16))
    return energy_estimation::TYPE_UINT16;
  if (type->isIntegerTy(32))
    return energy_estimation::TYPE_UINT32;
  if (type->isIntegerTy(64))
    return energy_estimation::TYPE_UINT64;
  if (type->isPointerTy())
    return energy_estimation::TYPE_PTR;

  return energy_estimation::TYPE_ERR;
}

std::string ProtoTransform::typeToString(llvm::Type* type) {
  if (!type) return "Void";

  if (type->isFloatTy())
    return "Float";
  if (type->isHalfTy())
    return "Half";
  if (type->isDoubleTy())
    return "Double";
  if (type->isIntegerTy(1))
    return "Bool";
  if (type->isIntegerTy(8))
    return "Int8";
  if (type->isIntegerTy(16))
    return "Int16";
  if (type->isIntegerTy(32))
    return "Int32";
  if (type->isIntegerTy(64))
    return "Int64";
  if (type->isPointerTy())
    return "Ptr";

  return typeStr(type);
}

energy_estimation::Instruction ProtoTransform::instToProto(const std::string &k, bool &isUniform) {
	if (EMDebugEnabled()) std::cout << "instToProto: " << k << std::endl;

	static constexpr llvm::StringLiteral kUniformSuffix = "_uniform";
	std::string base = k;
	isUniform = false;
	if (llvm::StringRef(k).ends_with(kUniformSuffix)) {
		isUniform = true;
		base = k.substr(0, k.size() - kUniformSuffix.size());
	}

  if (base == "fadd" || base == "add" || base == "__hadd") {
    return INST_ADD;
  }
  if (base == "fsub" || base == "sub" || base == "__hsub") {
    return INST_SUB;
  }
  if (base == "fmul" || base == "mul" || base == "__hmul") {
    return INST_MUL;
  }
	if (base == "fneg") {
		return INST_FNEG;
	}
	if (base == "fdiv") {
		return INST_DIV;
	}
	if (base == "srem" || base == "urem") {
		return INST_MOD;
	}
	if (base == "or") {
		return INST_OR;
	}
	if (base == "and") {
		return INST_AND;
	}
	if (base == "load") {
		return INST_MEMORY_OP;
	}
	if (base == "store") {
		// Generic memory op, not assumed to be an L1 hit -- CacheHitRateConfig later splits
		// the resulting MEMORY_OP count across L1/L2/main-memory.
		return INST_MEMORY_OP;
	}
	if (base == "shared_load") {
		return INST_SHARED_LOAD;
	}
	if (base == "shared_store") {
		//TODO: for now assume always store = load energy
		return INST_SHARED_LOAD;
	}
	if (base == "getelementptr") {
		return INST_GETELEMENTPTR_TYPED;
	}
	if (base == "icmp") {
		return INST_ICMP_TYPED;
	}
	if (base == "br") {
		return INST_BR;
	}
	if (base == "fpext" || base == "fptrunc") {
		return INST_FPDOUBLE_C;
	}
	if (base == "fptoui" || base == "fptosi") {
		return INST_FLOAT_TO_INT;
	}
	if (base == "fptoui_double" || base == "fptosi_double") {
		return INST_DOUBLE_TO_INT;
	}
	// Bit-manipulation intrinsics (llvm.<op>.i<N>), typed by their operand type like the benchmarks
	if (llvm::StringRef(base).starts_with("llvm.ctlz.")) {
		return INST_CTLZ;
	}
	if (llvm::StringRef(base).starts_with("llvm.cttz.")) {
		return INST_CTTZ;
	}
	if (llvm::StringRef(base).starts_with("llvm.ctpop.")) {
		return INST_CTPOP;
	}
	if (llvm::StringRef(base).starts_with("llvm.bswap.")) {
		return INST_BSWAP;
	}
	// Generic LLVM math intrinsics
	static const std::pair<llvm::StringLiteral, Instruction> mathIntrinsics[] = {
		{"llvm.sqrt.", INST_SQRT},
		{"llvm.pow.", INST_POW},
		{"llvm.exp.", INST_EXP},
		{"llvm.exp2.", INST_EXP2},
		{"llvm.exp10.", INST_EXP10},
		{"llvm.log.", INST_LOG},
		{"llvm.log2.", INST_LOG2},
		{"llvm.log10.", INST_LOG10},
		{"llvm.sin.", INST_SIN},
		{"llvm.cos.", INST_COS},
		{"llvm.tan.", INST_TAN},
		{"llvm.asin.", INST_ASIN},
		{"llvm.acos.", INST_ACOS},
		{"llvm.atan.", INST_ATAN},
		{"llvm.sinh.", INST_SINH},
		{"llvm.cosh.", INST_COSH},
		{"llvm.tanh.", INST_TANH},
	};
	for (const auto &[prefix, inst]: mathIntrinsics) {
		if (llvm::StringRef(base).starts_with(prefix)) {
			return inst;
		}
	}
  if (base.find("fma") != std::string::npos || base.find("fmuladd") != std::string::npos) {
    return INST_FMA;
  }
	if (base == "__nv_sinf" || base == "__nv_sin") {
		return INST_SIN;
	}
	if (base == "__nv_cosf" || base == "__nv_cos") {
		return INST_COS;
	}
	if (base == "__nv_tanf" || base == "__nv_tan") {
		return INST_TAN;
	}
	if (base == "__nv_logf" || base == "__nv_log" || base == "hlog") {
		return INST_LOG;
	}
	if (base == "__nv_log2f" || base == "__nv_log2" || base == "hlog2") {
		return INST_LOG2;
	}
	if (base == "__nv_log10f" || base == "__nv_log10" || base == "hlog10") {
		return INST_LOG10;
	}
	if (base == "__nv_expf" || base == "__nv_exp" || base == "hexp") {
		return INST_EXP;
	}
	if (base == "__nv_exp2f" || base == "__nv_exp2" || base == "hexp2") {
		return INST_EXP2;
	}
	if (base == "__nv_exp10f" || base == "__nv_exp10" || base == "hexp10") {
		return INST_EXP10;
	}
	if (base == "__nv_sinhf" || base == "__nv_sinh") {
		return INST_SINH;
	}
	if (base == "__nv_coshf" || base == "__nv_cosh") {
		return INST_COSH;
	}
	if (base == "__nv_tanhf" || base == "__nv_tanh" || base == "htanh") {
		return INST_TANH;
	}
	if (base == "__nv_asinf" || base == "__nv_asin") {
		return INST_ASIN;
	}
	if (base == "__nv_acosf" || base == "__nv_acos") {
		return INST_ACOS;
	}
	if (base == "__nv_atanf" || base == "__nv_atan") {
		return INST_ATAN;
	}
	if (base == "__nv_powf" || base == "__nv_pow") {
		return INST_POW;
	}
	if (base == "__nv_sqrtf" || base == "__nv_sqrt" || base == "hsqrt") {
		return INST_SQRT;
	}
  return INST_ERR;
}

energy_estimation::Instruction ProtoTransform::instToProto(const InstKey &k) {
  switch (k.opcode) {
    case llvm::Instruction::Add:
    case llvm::Instruction::FAdd:
      return energy_estimation::INST_ADD;
    case llvm::Instruction::Sub:
    case llvm::Instruction::FSub:
      return energy_estimation::INST_SUB;
    case llvm::Instruction::Mul:
    case llvm::Instruction::FMul:
      return energy_estimation::INST_MUL;
    case llvm::Instruction::Br:
      return energy_estimation::INST_BR;
    case llvm::Instruction::Store:
      return energy_estimation::INST_STORE;
    case llvm::Instruction::Load:
      return energy_estimation::INST_LOAD;
    case llvm::Instruction::Call:
      return energy_estimation::INST_CALL;
    case llvm::Instruction::Ret:
      return energy_estimation::INST_RET;
    case llvm::Instruction::FPToSI:
      return energy_estimation::INST_FPTOSI;
    case llvm::Instruction::Or:
      return energy_estimation::INST_OR;
    default:
      return energy_estimation::INST_ERR;
  }
}

std::string ProtoTransform::typeStr(llvm::Type *type) {
  std::string s;
  llvm::raw_string_ostream rso(s);
  type->print(rso);
  return rso.str();
}

std::string instName(const InstKey &k) {
	std::string name = llvm::Instruction::getOpcodeName(k.opcode);

	// ICMP predicate handling
	if (k.opcode == llvm::Instruction::ICmp) {
		auto pred = static_cast<llvm::ICmpInst::Predicate>(k.subcode);
		name += llvm::ICmpInst::getPredicateName(pred).str();
	}

	if (!name.empty() && (name[0] == 'f' || name[0] == 'F')) {

		// only strip if it matches known FP ops
		std::string lower = name;
		lower[0] = std::tolower(lower[0]);

		if (lower.rfind("fadd", 0) == 0) name = name.substr(1);
		else if (lower.rfind("fsub", 0) == 0) name = name.substr(1);
		else if (lower.rfind("fmul", 0) == 0) name = name.substr(1);
		else if (lower.rfind("fdiv", 0) == 0) name = name.substr(1);
		else if (lower.rfind("frem", 0) == 0) name = name.substr(1);
	}

	return capitalize(name);
}
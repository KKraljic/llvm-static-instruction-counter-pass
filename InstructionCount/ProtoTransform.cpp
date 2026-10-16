//
// Created by nick on 7/5/26.
//

#include "DebugToggle.hpp"
#include "ProtoTransform.hpp"

#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>

#include <utility>

using namespace energy_estimation;

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
	{"__habs", TYPE_FP16},
	// br benchmark only registered UINT32
	{"br",      TYPE_UINT32},
};

const llvm::StringLiteral ProtoTransform::inlineAsmPrefix = "asm:";

static llvm::StringRef inlineAsmMnemonic(llvm::StringRef inst) {
	return inst.drop_front(ProtoTransform::inlineAsmPrefix.size()).split('_').first;
}

static bool isF16Ptx(llvm::StringRef mnemonic) {
	llvm::SmallVector<llvm::StringRef> parts;
	mnemonic.split(parts, '.');
	return llvm::is_contained(parts, "f16") || llvm::is_contained(parts, "f16x2");
}

// Some ptx instructions, everything else is INST_ERR for the moment and not covered
static Instruction ptxToProto(llvm::StringRef mnemonic) {
	if (!isF16Ptx(mnemonic)) return INST_ERR;
	static const llvm::StringMap<Instruction> ptxOps{
		{"add", INST_ADD},
		{"sub", INST_SUB},
		{"mul", INST_MUL},
		{"fma", INST_FMA},
		{"neg", INST_FNEG},
		{"abs", INST_ABS},
		{"ex2", INST_EXP2},
	};
	auto it = ptxOps.find(mnemonic.split('.').first);
	return it == ptxOps.end() ? INST_ERR : it->second;
}

std::string countedInlineAsmName(const llvm::Instruction &inst) {
	const auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
	if (!call || !call->isInlineAsm()) return "";

	llvm::StringRef asmStr = llvm::cast<llvm::InlineAsm>(call->getCalledOperand())->getAsmString();
	asmStr = asmStr.trim(" \t\r\n{}");
	if (asmStr.contains('\n') || asmStr.count(';') != 1 || !asmStr.ends_with(";")) return "";

	std::string mnemonic = asmStr.take_until([](char c) { return c == ' ' || c == '\t'; }).str();
	if (inst.getMetadata("dbg.pass.fma") && isF16Ptx(mnemonic)) {
		mnemonic = "fma" + mnemonic.substr(mnemonic.find('.'));
	}
	const std::string name = (ProtoTransform::inlineAsmPrefix + mnemonic).str();
	bool isUniform = false;
	if (ProtoTransform::instToProto(name, isUniform) == INST_ERR) return "";
	return name;
}

ValueType ProtoTransform::typeToProto(const std::string &type, const std::string &inst) {
	if (ProtoTransform::InstTypeMap.contains(inst)) {
		return ProtoTransform::InstTypeMap.at(inst);
	}

	if (llvm::StringRef(inst).starts_with(inlineAsmPrefix) && isF16Ptx(inlineAsmMnemonic(inst))) {
		return TYPE_FP16;
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

  // map i8/16 conversion to 32-bit
  {
    const llvm::StringRef instRef(inst);
    if ((instRef.starts_with("fptosi") || instRef.starts_with("fptoui") || instRef.starts_with("sitofp") ||
         instRef.starts_with("uitofp")) &&
        (elemType == "i8" || elemType == "i16")) {
      elemType = "i32";
    }
  }

  if (llvm::StringRef(inst).starts_with("fptosi") || llvm::StringRef(inst).starts_with("sitofp") ||
      llvm::StringRef(inst).starts_with("srem") ||
      llvm::StringRef(inst).starts_with("sdiv") || llvm::StringRef(inst).starts_with("ashr")) {
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

	constexpr llvm::StringLiteral uniformSuffix = "_uniform";
	std::string base = k;
	isUniform = false;
	if (llvm::StringRef(k).ends_with(uniformSuffix)) {
		isUniform = true;
		base = k.substr(0, k.size() - uniformSuffix.size());
	}

	if (llvm::StringRef(base).starts_with(inlineAsmPrefix)) {
		return ptxToProto(inlineAsmMnemonic(base));
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
	if (base == "fdiv" || base == "udiv" || base == "sdiv") {
		return INST_DIV;
	}
	if (base == "__nv_fabsf" || base == "__nv_fabs" || base == "__habs") {
		return INST_ABS;
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
	if (base == "xor") {
		return INST_XOR;
	}
	if (base == "shl") {
		return INST_SHL;
	}
	// lshr typed unsigned, ashr signed (see typeToProto)
	if (base == "lshr" || base == "ashr") {
		return INST_SHR;
	}
	if (base == "load") {
		return INST_MEMORY_OP;
	}
	if (base == "store") {
		// Generic memory op, CacheHitRateConfig later splits
		// the MEMORY_OP count across L1/L2/main-memory.
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

	if (base == "uitofp" || base == "sitofp") {
		return INST_INT_TO_FLOAT;
	}
	if (base == "uitofp_double" || base == "sitofp_double") {
		return INST_INT_TO_DOUBLE;
	}

	if (llvm::StringRef(base).starts_with("llvm.ctlz.") || base == "__nv_clz" || base == "__nv_clzll") {
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
		{"llvm.fabs.", INST_ABS},
		{"llvm.sqrt.", INST_SQRT},
		{"llvm.pow.", INST_POW},
		{"llvm.exp.", INST_EXP_REAL},
		{"llvm.exp2.", INST_EXP2},
		{"llvm.exp10.", INST_EXP10},
		{"llvm.log.", INST_LOG_REAL},
		{"llvm.log2.", INST_LOG2},
		{"llvm.log10.", INST_LOG10},
		{"llvm.sin.", INST_SIN},
		{"llvm.cos.", INST_COS},
		{"llvm.tan.", INST_TAN},
		{"llvm.asin.", INST_ASIN_REAL},
		{"llvm.acos.", INST_ACOS_REAL},
		{"llvm.atan.", INST_ATAN_REAL},
		{"llvm.sinh.", INST_SINH_REAL},
		{"llvm.cosh.", INST_COSH_REAL},
		{"llvm.tanh.", INST_TANH_REAL},
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
		return INST_LOG_REAL;
	}
	if (base == "__nv_log2f" || base == "__nv_log2" || base == "hlog2") {
		return INST_LOG2;
	}
	if (base == "__nv_log10f" || base == "__nv_log10" || base == "hlog10") {
		return INST_LOG10;
	}
	if (base == "__nv_expf" || base == "__nv_exp" || base == "hexp") {
		return INST_EXP_REAL;
	}
	if (base == "__nv_exp2f" || base == "__nv_exp2" || base == "hexp2") {
		return INST_EXP2;
	}
	if (base == "__nv_exp10f" || base == "__nv_exp10" || base == "hexp10") {
		return INST_EXP10;
	}
	if (base == "__nv_sinhf" || base == "__nv_sinh") {
		return INST_SINH_REAL;
	}
	if (base == "__nv_coshf" || base == "__nv_cosh") {
		return INST_COSH_REAL;
	}
	if (base == "htanh") {
		// INST_TANH_REAL needs INST_TANH_TEST, which only exists for float/double
		return INST_TANH;
	}
	if (base == "__nv_tanhf" || base == "__nv_tanh") {
		return INST_TANH_REAL;
	}
	if (base == "__nv_asinf" || base == "__nv_asin") {
		return INST_ASIN_REAL;
	}
	if (base == "__nv_acosf" || base == "__nv_acos") {
		return INST_ACOS_REAL;
	}
	if (base == "__nv_atanf" || base == "__nv_atan") {
		return INST_ATAN_REAL;
	}
	if (base == "__nv_powf" || base == "__nv_pow") {
		return INST_POW;
	}
	if (base == "__nv_sqrtf" || base == "__nv_sqrt" || base == "hsqrt") {
		return INST_SQRT;
	}
  return INST_ERR;
}

std::string ProtoTransform::typeStr(llvm::Type *type) {
  std::string s;
  llvm::raw_string_ostream rso(s);
  type->print(rso);
  return rso.str();
}

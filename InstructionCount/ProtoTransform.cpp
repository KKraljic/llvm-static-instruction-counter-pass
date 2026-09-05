//
// Created by nick on 7/5/26.
//

#include "DebugToggle.hpp"
#include "ProtoTransform.hpp"

#include <llvm/IR/Instructions.h>

const llvm::StringMap<ValueType> ProtoTransform::InstTypeMap {
	{"__hadd",  TYPE_FP16},
	{"__hsub",  TYPE_FP16},
	{"__hmul",  TYPE_FP16},
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
  if (base.find("fma") != std::string::npos || base.find("fmuladd") != std::string::npos) {
    return INST_FMA;
  }
	if (base == "__nv_sinf" || base == "__nv_sin") {
		return INST_SIN;
	}
	if (base == "__nv_cosf" || base == "__nv_cos") {
		return INST_COS;
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
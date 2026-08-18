//
// Created by nick on 7/5/26.
//

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

	std::cout << "typeToProto: " << type << std::endl;
  if (type == "float") return TYPE_FP32;
  if (type == "double") return TYPE_FP64;
  if (type == "half") return TYPE_FP16;
  if (type == "i32") return TYPE_INT32;
  if (type == "i64") return TYPE_INT64;
  if (type == "i16") return TYPE_INT16;
	if (type.find("ptr") != std::string::npos) {
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
    return energy_estimation::TYPE_INT8;
  if (type->isIntegerTy(16))
    return energy_estimation::TYPE_INT16;
  if (type->isIntegerTy(32))
    return energy_estimation::TYPE_INT32;
  if (type->isIntegerTy(64))
    return energy_estimation::TYPE_INT64;
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

energy_estimation::Instruction ProtoTransform::instToProto(const std::string &k) {
	std::cout << "instToProto: " << k << std::endl;
  if (k == "fadd" || k == "add" || k == "__hadd") {
    return INST_ADD;
  }
  if (k == "fsub" || k == "sub" || k == "__hsub") {
    return INST_SUB;
  }
  if (k == "fmul" || k == "mul" || k == "__hmul") {
    return INST_MUL;
  }
	if (k == "or") {
		return INST_OR;
	}
	if (k == "load") {
		return INST_LOAD;
	}
	if (k == "store") {
		return INST_STORE;
	}
	if (k == "shared_load") {
		return INST_SHARED_LOAD;
	}
	if (k == "shared_store") {
		return INST_SHARED_STORE;
	}
  if (k.find("fma") != std::string::npos) {
    return INST_FMA;
  }
	if (k == "__nv_sinf" || k == "__nv_sin") {
		return INST_SIN;
	}
	if (k == "__nv_cosf" || k == "__nv_cos") {
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
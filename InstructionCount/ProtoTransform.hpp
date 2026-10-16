//
// Created by nick on 7/5/26.
//

#ifndef EPI_PEN_PROTOTRANSFORM_H
#define EPI_PEN_PROTOTRANSFORM_H
#include <llvm/ADT/StringMap.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Type.h>

#include "interface.pb.h"

#include <string>

class ProtoTransform {
public:
  static const llvm::StringLiteral inlineAsmPrefix;

  static energy_estimation::ValueType typeToProto(const std::string &type, const std::string &inst);
  static energy_estimation::ValueType typeToProto(llvm::Type* type);
  static std::string typeToString(llvm::Type* type);
  static energy_estimation::Instruction instToProto(const std::string &k, bool &isUniform);
private:
	static const llvm::StringMap<energy_estimation::ValueType> InstTypeMap;
  static std::string typeStr(llvm::Type *type);
};

// Opcode name for a call to one-statement inline asm (fp16 ops are only inline asm)
std::string countedInlineAsmName(const llvm::Instruction &inst);

#endif //EPI_PEN_PROTOTRANSFORM_H

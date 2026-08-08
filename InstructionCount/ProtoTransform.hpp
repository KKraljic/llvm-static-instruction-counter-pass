//
// Created by nick on 7/5/26.
//

#ifndef EPI_PEN_PROTOTRANSFORM_H
#define EPI_PEN_PROTOTRANSFORM_H
#include <llvm/ADT/StringMap.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Type.h>

#include "interface.pb.h"

using namespace energy_estimation;

struct InstKey {
  unsigned opcode;
  unsigned subcode;

  bool operator<(const InstKey& other) const {
    return std::tie(opcode, subcode) <
           std::tie(other.opcode, other.subcode);
  }
};

class ProtoTransform {
public:
  static energy_estimation::ValueType typeToProto(const std::string &type, const std::string &inst);
  static energy_estimation::ValueType typeToProto(llvm::Type* type);
  static std::string typeToString(llvm::Type* type);
  static energy_estimation::Instruction instToProto(const std::string &k);
  static energy_estimation::Instruction instToProto(const InstKey &k);
private:
	static const llvm::StringMap<ValueType> InstTypeMap;
  static std::string typeStr(llvm::Type *type);
	std::string instName(const InstKey &k);
};


#endif //EPI_PEN_PROTOTRANSFORM_H

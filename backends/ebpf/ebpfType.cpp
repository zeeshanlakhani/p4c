/*
Copyright 2013-present Barefoot Networks, Inc.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include "ebpfType.h"

namespace EBPF {

EBPFTypeFactory* EBPFTypeFactory::instance;

EBPFType* EBPFTypeFactory::create(const IR::Type* type) {
    CHECK_NULL(type);
    CHECK_NULL(typeMap);
    EBPFType* result = nullptr;

    if (type->is<IR::Type_Boolean>()) {
        result = new EBPFBoolType();
    } else if (type->is<IR::Type_Bits>()) {
        result = new EBPFScalarType(type->to<IR::Type_Bits>());
    } else if (type->is<IR::Type_StructLike>()) {
        result = new EBPFStructType(type->to<IR::Type_StructLike>());
    } else if (type->is<IR::Type_Stack>()) {
        result = new EBPFStackType(type->to<IR::Type_Stack>());
    } else if (type->is<IR::Type_Typedef>()) {
        auto canon = typeMap->getTypeType(type, true);
        result = create(canon);
        auto path = new IR::Path(type->to<IR::Type_Typedef>()->name);
        result = new EBPFTypeName(new IR::Type_Name(path), result);
    } else if (type->is<IR::Type_Name>()) {
        auto canon = typeMap->getTypeType(type, true);
        result = create(canon);
        result = new EBPFTypeName(type->to<IR::Type_Name>(), result);
    } else if (type->is<IR::Type_Enum>()) {
        return new EBPFEnumType(type->to<IR::Type_Enum>());
    } else {
        ::error("Type %1% not supported", type);
    }

    return result;
}

void EBPFBoolType::declare(CodeBuilder* builder, cstring id, bool asPointer) {
    emit(builder);
    if (asPointer)
        builder->append("*");
    builder->appendFormat(" %s", id.c_str());
}

/////////////////////////////////////////////////////////////

unsigned EBPFScalarType::alignment() const {
    if (width <= 8)
        return 1;
    else if (width <= 16)
        return 2;
    else if (width <= 32)
        return 4;
    else if (width <= 64)
        return 8;
    else
        // compiled as u8*
        return 1;
}

void EBPFScalarType::emit(CodeBuilder* builder) {
    auto prefix = isSigned ? "i" : "u";

    if (width <= 8)
        builder->appendFormat("%s8", prefix);
    else if (width <= 16)
        builder->appendFormat("%s16", prefix);
    else if (width <= 32)
        builder->appendFormat("%s32", prefix);
    else if (width <= 64)
        builder->appendFormat("%s64", prefix);
    else
        builder->appendFormat("u8*");
}

void
EBPFScalarType::declare(CodeBuilder* builder, cstring id, bool asPointer) {
    if (EBPFScalarType::generatesScalar(width)) {
        emit(builder);
        if (asPointer)
            builder->append("*");
        builder->spc();
        builder->append(id);
    } else {
        if (asPointer)
            builder->append("u8*");
        else
            builder->appendFormat("u8 %s[%d]", id.c_str(), bytesRequired());
    }
}

//////////////////////////////////////////////////////////

EBPFStructType::EBPFStructType(const IR::Type_StructLike* strct) : EBPFType(strct) {
    name = strct->name.name;
    width = 0;
    implWidth = 0;

    if (strct->is<IR::Type_Struct>())
        kind = "struct";
    else if (strct->is<IR::Type_Header>())
        kind = "struct";
    else if (strct->is<IR::Type_HeaderUnion>())
        kind = "union";
    else
        BUG("Unexpected struct type %1%", strct);

    for (auto f : strct->fields) {
        auto type = EBPFTypeFactory::instance->create(f->type);
        auto wt = dynamic_cast<IHasWidth*>(type);
        if (wt == nullptr) {
            ::error("EBPF: Unsupported type in struct: %s", f->type);
        } else {
            width += wt->widthInBits();
            implWidth += wt->implementationWidthInBits();
        }
        fields.push_back(new EBPFField(type, f));
    }
}

void EBPFStructType::declare(CodeBuilder* builder, cstring id, bool asPointer) {
    builder->append(kind);
    if (asPointer)
        builder->append("*");
    const char* n = name.c_str();
    builder->appendFormat(" %s %s", n, id.c_str());
}

void EBPFStructType::emitInitializer(CodeBuilder* builder) {
    builder->blockStart();
    if (type->is<IR::Type_Struct>() || type->is<IR::Type_HeaderUnion>()) {
        for (auto f : fields) {
            builder->emitIndent();
            builder->appendFormat(".%s = ", f->field->name.name);
            f->type->emitInitializer(builder);
            builder->append(",");
            builder->newline();
        }
    } else if (type->is<IR::Type_Header>()) {
        builder->emitIndent();
        builder->appendLine(".ebpf_valid = 0");
    } else {
        BUG("Unexpected type %1%", type);
    }
    builder->blockEnd(false);
}

void EBPFStructType::emit(CodeBuilder* builder) {
    builder->emitIndent();
    builder->append(kind);
    builder->spc();
    builder->append(name);
    builder->spc();
    builder->blockStart();

    for (auto f : fields) {
        auto type = f->type;
        builder->emitIndent();

        type->declare(builder, f->field->name, false);
        builder->append("; ");
        builder->append("/* ");
        builder->append(type->type->toString());
        if (f->comment != nullptr) {
            builder->append(" ");
            builder->append(f->comment);
        }
        builder->append(" */");
        builder->newline();
    }

    if (type->is<IR::Type_Header>()) {
        builder->emitIndent();
        auto type = EBPFTypeFactory::instance->create(IR::Type_Boolean::get());
        if (type != nullptr) {
            type->declare(builder, "ebpf_valid", false);
            builder->endOfStatement(true);
        }
    }

    builder->blockEnd(false);
    builder->endOfStatement(true);
}

///////////////////////////////////////////////////////////////

void EBPFTypeName::declare(CodeBuilder* builder, cstring id, bool asPointer) {
    if (canonical != nullptr)
        canonical->declare(builder, id, asPointer);
}

void EBPFTypeName::emitInitializer(CodeBuilder* builder) {
    if (canonical != nullptr)
        canonical->emitInitializer(builder);
}

unsigned EBPFTypeName::widthInBits() {
    auto wt = dynamic_cast<IHasWidth*>(canonical);
    if (wt == nullptr) {
        ::error("Type %1% does not have a fixed witdh", type);
        return 0;
    }
    return wt->widthInBits();
}

unsigned EBPFTypeName::implementationWidthInBits() {
    auto wt = dynamic_cast<IHasWidth*>(canonical);
    if (wt == nullptr) {
        ::error("Type %1% does not have a fixed witdh", type);
        return 0;
    }
    return wt->implementationWidthInBits();
}

////////////////////////////////////////////////////////////////

void EBPFEnumType::declare(EBPF::CodeBuilder* builder, cstring id, bool asPointer) {
    builder->append("enum ");
    builder->append(getType()->name);
    if (asPointer)
        builder->append("*");
    builder->append(" ");
    builder->append(id);
}

void EBPFEnumType::emit(EBPF::CodeBuilder* builder) {
    builder->append("enum ");
    auto et = getType();
    builder->append(et->name);
    builder->blockStart();
    for (auto m : et->members) {
        builder->append(m->name);
        builder->appendLine(",");
    }
    builder->blockEnd(true);
}

////////////////////////////////////////////////////////////////

EBPFStackType::EBPFStackType(const IR::Type_Stack* stack) : EBPFType(stack) {
    hdr = stack->elementType;
    hdrType = EBPFTypeFactory::instance->create(hdr);
    stackSize = stack->getSize();
    auto wt = dynamic_cast<IHasWidth*>(hdrType);
    width = wt->widthInBits() * stackSize;
    implWidth = wt->implementationWidthInBits() * stackSize;
}

void EBPFStackType::declare(CodeBuilder* builder, cstring id, bool asPointer) {
    (void)asPointer;
    hdrType->declare(builder, id, false);
    builder->appendFormat("[%d]", stackSize);
}

void EBPFStackType::emitInitializer(CodeBuilder* builder) {
    builder->blockStart();
    for (unsigned i = 0; i < stackSize; i++) {
        builder->emitIndent();
        hdrType->emitInitializer(builder);
        builder->append(",");
        builder->newline();
    }
    builder->blockEnd(false);
}

}  // namespace EBPF

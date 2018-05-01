//===-- SymbolDumper.cpp - CodeView symbol info dumper ----------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/DebugInfo/CodeView/SymbolDumper.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/DebugInfo/CodeView/CVSymbolVisitor.h"
#include "llvm/DebugInfo/CodeView/DebugStringTableSubsection.h"
#include "llvm/DebugInfo/CodeView/EnumTables.h"
#include "llvm/DebugInfo/CodeView/SymbolDeserializer.h"
#include "llvm/DebugInfo/CodeView/SymbolDumpDelegate.h"
#include "llvm/DebugInfo/CodeView/SymbolRecord.h"
#include "llvm/DebugInfo/CodeView/SymbolVisitorCallbackPipeline.h"
#include "llvm/DebugInfo/CodeView/SymbolVisitorCallbacks.h"
#include "llvm/DebugInfo/CodeView/TypeIndex.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ScopedPrinter.h"

#include <system_error>

using namespace llvm;
using namespace llvm::codeview;

namespace {
/// Use this private dumper implementation to keep implementation details about
/// the visitor out of SymbolDumper.h.
class CVSymbolDumperImpl : public SymbolVisitorCallbacks {
public:
  CVSymbolDumperImpl(TypeCollection &Types, SymbolDumpDelegate *ObjDelegate,
                     ScopedPrinter &W, bool PrintRecordBytes)
      : Types(Types), ObjDelegate(ObjDelegate), W(W),
        PrintRecordBytes(PrintRecordBytes), InFunctionScope(false) {}

/// CVSymbolVisitor overrides.
#define SYMBOL_RECORD(EnumName, EnumVal, Name)                                 \
  Error visitKnownRecord(CVSymbol &CVR, Name &Record) override;
#define SYMBOL_RECORD_ALIAS(EnumName, EnumVal, Name, AliasName)
#include "llvm/DebugInfo/CodeView/CodeViewSymbols.def"

  Error visitSymbolBegin(CVSymbol &Record) override;
  Error visitSymbolEnd(CVSymbol &Record) override;
  Error visitUnknownSymbol(CVSymbol &Record) override;

private:
  void printLocalVariableAddrRange(const LocalVariableAddrRange &Range,
                                   uint32_t RelocationOffset);
  void printLocalVariableAddrGap(ArrayRef<LocalVariableAddrGap> Gaps);
  void printTypeIndex(StringRef FieldName, TypeIndex TI);

  TypeCollection &Types;
  SymbolDumpDelegate *ObjDelegate;
  ScopedPrinter &W;

  bool PrintRecordBytes;
  bool InFunctionScope;
};
}

static StringRef getSymbolKindName(SymbolKind Kind) {
  switch (Kind) {
#define SYMBOL_RECORD(EnumName, EnumVal, Name)                                 \
  case EnumName:                                                               \
    return #Name;
#include "llvm/DebugInfo/CodeView/CodeViewSymbols.def"
  default:
    break;
  }
  return "UnknownSym";
}

void CVSymbolDumperImpl::printLocalVariableAddrRange(
    const LocalVariableAddrRange &Range, uint32_t RelocationOffset) {
  DictScope S(W, "LocalVariableAddrRange");
  if (ObjDelegate)
    ObjDelegate->printRelocatedField("OffsetStart", RelocationOffset,
                                     Range.OffsetStart);
  W.printHex("ISectStart", Range.ISectStart);
  W.printHex("Range", Range.Range);
}

void CVSymbolDumperImpl::printLocalVariableAddrGap(
    ArrayRef<LocalVariableAddrGap> Gaps) {
  for (auto &Gap : Gaps) {
    ListScope S(W, "LocalVariableAddrGap");
    W.printHex("GapStartOffset", Gap.GapStartOffset);
    W.printHex("Range", Gap.Range);
  }
}

void CVSymbolDumperImpl::printTypeIndex(StringRef FieldName, TypeIndex TI) {
  codeview::printTypeIndex(W, FieldName, TI, Types);
}

Error CVSymbolDumperImpl::visitSymbolBegin(CVSymbol &CVR) {
  W.startLine() << getSymbolKindName(CVR.Type);
  W.getOStream() << " {\n";
  W.indent();
  W.printEnum("Kind", unsigned(CVR.Type), getSymbolTypeNames());
  return Error::success();
}

Error CVSymbolDumperImpl::visitSymbolEnd(CVSymbol &CVR) {
  if (PrintRecordBytes && ObjDelegate)
    ObjDelegate->printBinaryBlockWithRelocs("SymData", CVR.content());

  W.unindent();
  W.startLine() << "}\n";
  return Error::success();
}

Error CVSymbolDumperImpl::visitKnownRecord(CVSymbol &CVR, BlockSym &Block) {
  StringRef LinkageName;
  W.printHex("PtrParent", Block.Parent);
  W.printHex("PtrEnd", Block.End);
  W.printHex("CodeSize", Block.CodeSize);
  if (ObjDelegate) {
    ObjDelegate->printRelocatedField("CodeOffset", Block.getRelocationOffset(),
                                     Block.CodeOffset, &LinkageName);
  }
  W.printHex("Segment", Block.Segment);
  W.printString("BlockName", Block.Name);
  W.printString("LinkageName", LinkageName);
  return Error::success();
}

Error CVSymbolDumperImpl::visitKnownRecord(CVSymbol &CVR, Thunk32Sym &Thunk) {
  W.printNumber("Parent", Thunk.Parent);
  W.printNumber("End", Thunk.End);
  W.printNumber("Next", Thunk.Next);
  W.printNumber("Off", Thunk.Offset);
  W.printNumber("Seg", Thunk.Segment);
  W.printNumber("Len", Thunk.Length);
  W.printEnum("Ordinal", uint8_t(Thunk.Thunk), getThunkOrdinalNames());
  return Error::success();
}

Error CVSymbolDumperImpl::visitKnownRecord(CVSymbol &CVR,
                                           TrampolineSym &Tramp) {
  W.printEnum("Type", uint16_t(Tramp.Type), getTrampolineNames());
  W.printNumber("Size", Tramp.Size);
  W.printNumber("ThunkOff", Tramp.ThunkOffset);
  W.printNumber("TargetOff", Tramp.TargetOffset);
  W.printNumber("ThunkSection", Tramp.ThunkSection);
  W.printNumber("TargetSection", Tramp.TargetSection);
  return Error::success();
}

Error CVSymbolDumperImpl::visitKnownRecord(CVSymbol &CVR, SectionSym &Section) {
  W.printNumber("SectionNumber", Section.SectionNumber);
  W.printNumber("Alignment", Section.Alignment);
  W.printNumber("Rva", Section.Rva);
  W.printNumber("Length", Section.Length);
  W.printFlags("Characteristics", Section.Characteristics,
               getImageSectionCharacteristicNames(),
               COFF::SectionCharacteristics(0x00F00000));

  W.printString("Name", Section.Name);
  return Error::success();
}

Error CVSymbolDumperImpl::visitKnownRecord(CVSymbol &CVR,
                                           CoffGroupSym &CoffGroup) {
  W.printNumber("Size", CoffGroup.Size);
  W.printFlags("Characteristics", CoffGroup.Characteristics,
               getImageSectionCharacteristicNames(),
               COFF::SectionCharacteristics(0x00F00000));
  W.printNumber("Offset", CoffGroup.Offset);
  W.printNumber("Segment", CoffGroup.Segment);
  W.printString("Name", CoffGroup.Name);
  return Error::success();
}

Error CVSymbolDumperImpl::visitKnownRecord(CVSymbol &CVR,
                                           BPRelativeSym &BPRel) {
  W.printNumber("Offset", BPRel.Offset);
  printTypeIndex("Type", BPRel.Type);
  W.printString("VarName", BPRel.Name);
  return Error::success();
}

Error CVSymbolDumperImpl::visitKnownRecord(CVSymbol &CVR,
                                           BuildInfoSym &BuildInfo) {
  printTypeIndex("BuildId", BuildInfo.BuildId);
  return Error::success();
}

Error CVSymbolDumperImpl::visitKnownRecord(CVSymbol &CVR,
                                           CallSiteInfoSym &CallSiteInfo) {
  StringRef LinkageName;
  if (ObjDelegate) {
    ObjDelegate->printRelocatedField("CodeOffset",
                                     CallSiteInfo.getRelocationOffset(),
                                     CallSiteInfo.CodeOffset, &LinkageName);
  }
  W.printHex("Segment", CallSiteInfo.Segment);
  printTypeIndex("Type", CallSiteInfo.Type);
  if (!LinkageName.empty())
    W.printString("LinkageName", LinkageName);
  return Error::success();
}

Error CVSymbolDumperImpl::visitKnownRecord(CVSymbol &CVR,
                                           EnvBlockSym &EnvBlock) {
  ListScope L(W, "Entries");
  for (auto Entry : EnvBlock.Fields) {
    W.printString(Entry);
  }
  return Error::success();
}

Error CVSymbolDumperImpl::visitKnownRecord(CVSymbol &CVR,
                                           FileStaticSym &FileStatic) {
  printTypeIndex("Index", FileStatic.Index);
  W.printNumber("ModFilenameOffset", FileStatic.ModFilenameOffset);
  W.printFlags("Flags", uint16_t(FileStatic.Flags), getLocalFlagNames());
  W.printString("Name", FileStatic.Name);
  return Error::success();
}

Error CVSymbolDumperImpl::visitKnownRecord(CVSymbol &CVR, ExportSym &Export) {
  W.printNumber("Ordinal", Export.Ordinal);
  W.printFlags("Flags", uint16_t(Export.Flags), getExportSymFlagNames());
  W.printString("Name", Export.Name);
  return Error::success();
}

Error CVSymbolDumperImpl::visitKnownRecord(CVSymbol &CVR,
                                           Compile2Sym &Compile2) {
  W.printEnum("Language", Compile2.getLanguage(), getSourceLanguageNames());
  W.printFlags("Flags", Compile2.getFlags(), getCompileSym2FlagNames());
  W.printEnum("Machine", unsigned(Compile2.Machine), getCPUTypeNames());
  std::string FrontendVersion;
  {
    raw_string_ostream Out(FrontendVersion);
    Out << Compile2.VersionFrontendMajor << '.' << Compile2.VersionFrontendMinor
        << '.' << Compile2.VersionFrontendBuild;
  }
  std::string BackendVersion;
  {
    raw_string_ostream Out(BackendVersion);
    Out << Compile2.VersionBackendMajor << '.' << Compile2.VersionBackendMinor
        << '.' << Compile2.VersionBackendBuild;
  }
  W.printString("FrontendVersion", FrontendVersion);
  W.printString("BackendVersion", BackendVersion);
  W.printString("VersionName", Compile2.Version);
  return Error::success();
}

Error CVSymbolDumperImpl::visitKnownRecord(CVSymbol &CVR,
                                           Compile3Sym &Compile3) {
  W.printEnum("Language", Compile3.getLanguage(), getSourceLanguageNames());
  W.printFlags("Flags", Compile3.getFlags(), getCompileSym3FlagNames());
  W.printEnum("Machine", unsigned(Compile3.Machine), getCPUTypeNames());
  std::string FrontendVersion;
  {
    raw_string_ostream Out(FrontendVersion);
    Out << Compile3.VersionFrontendMajor << '.' << Compile3.VersionFrontendMinor
        << '.' << Compile3.VersionFrontendBuild << '.'
        << Compile3.VersionFrontendQFE;
  }
  std::string BackendVersion;
  {
    raw_string_ostream Out(BackendVersion);
    Out << Compile3.VersionBackendMajor << '.' << Compile3.VersionBackendMinor
        << '.' << Compile3.VersionBackendBuild << '.'
        << Compile3.VersionBackendQFE;
  }
  W.printString("FrontendVersion", FrontendVersion);
  W.printString("BackendVersion", BackendVersion);
  W.printString("VersionName", Compile3.Version);
  return Error::success();
}

Error CVSymbolDumperImpl::visitKnownRecord(CVSymbol &CVR,
                                           ConstantSym &Constant) {
  printTypeIndex("Type", Constant.Type);
  W.printNumber("Value", Constant.Value);
  W.printString("Name", Constant.Name);
  return Error::success();
}

Error CVSymbolDumperImpl::visitKnownRecord(CVSymbol &CVR, DataSym &Data) {
  StringRef LinkageName;
  if (ObjDelegate) {
    ObjDelegate->printRelocatedField("DataOffset", Data.getRelocationOffset(),
                                     Data.DataOffset, &LinkageName);
  }
  printTypeIndex("Type", Data.Type);
  W.printString("DisplayName", Data.Name);
  if (!LinkageName.empty())
    W.printString("LinkageName", LinkageName);
  return Error::success();
}

Error CVSymbolDumperImpl::visitKnownRecord(
    CVSymbol &CVR,
    DefRangeFramePointerRelFullScopeSym &DefRangeFramePointerRelFullScope) {
  W.printNumber("Offset", DefRangeFramePointerRelFullScope.Offset);
  return Error::success();
}

Error CVSymbolDumperImpl::visitKnownRecord(
    CVSymbol &CVR, DefRangeFramePointerRelSym &DefRangeFramePointerRel) {
  W.printNumber("Offset", DefRangeFramePointerRel.Offset);
  printLocalVariableAddrRange(DefRangeFramePointerRel.Range,
                              DefRangeFramePointerRel.getRelocationOffset());
  printLocalVariableAddrGap(DefRangeFramePointerRel.Gaps);
  return Error::success();
}

Error CVSymbolDumperImpl::visitKnownRecord(
    CVSymbol &CVR, DefRangeRegisterRelSym &DefRangeRegisterRel) {
  W.printEnum("BaseRegister", uint16_t(DefRangeRegisterRel.Hdr.Register),
              getRegisterNames());
  W.printBoolean("HasSpilledUDTMember",
                 DefRangeRegisterRel.hasSpilledUDTMember());
  W.printNumber("OffsetInParent", DefRangeRegisterRel.offsetInParent());
  W.printNumber("BasePointerOffset", DefRangeRegisterRel.Hdr.BasePointerOffset);
  printLocalVariableAddrRange(DefRangeRegisterRel.Range,
                              DefRangeRegisterRel.getRelocationOffset());
  printLocalVariableAddrGap(DefRangeRegisterRel.Gaps);
  return Error::success();
}

Error CVSymbolDumperImpl::visitKnownRecord(
    CVSymbol &CVR, DefRangeRegisterSym &DefRangeRegister) {
  W.printEnum("Register", uint16_t(DefRangeRegister.Hdr.Register),
              getRegisterNames());
  W.printNumber("MayHaveNoName", DefRangeRegister.Hdr.MayHaveNoName);
  printLocalVariableAddrRange(DefRangeRegister.Range,
                              DefRangeRegister.getRelocationOffset());
  printLocalVariableAddrGap(DefRangeRegister.Gaps);
  return Error::success();
}

Error CVSymbolDumperImpl::visitKnownRecord(
    CVSymbol &CVR, DefRangeSubfieldRegisterSym &DefRangeSubfieldRegister) {
  W.printEnum("Register", uint16_t(DefRangeSubfieldRegister.Hdr.Register),
              getRegisterNames());
  W.printNumber("MayHaveNoName", DefRangeSubfieldRegister.Hdr.MayHaveNoName);
  W.printNumber("OffsetInParent", DefRangeSubfieldRegister.Hdr.OffsetInParent);
  printLocalVariableAddrRange(DefRangeSubfieldRegister.Range,
                              DefRangeSubfieldRegister.getRelocationOffset());
  printLocalVariableAddrGap(DefRangeSubfieldRegister.Gaps);
  return Error::success();
}

Error CVSymbolDumperImpl::visitKnownRecord(
    CVSymbol &CVR, DefRangeSubfieldSym &DefRangeSubfield) {
  if (ObjDelegate) {
    DebugStringTableSubsectionRef Strings = ObjDelegate->getStringTable();
    auto ExpectedProgram = Strings.getString(DefRangeSubfield.Program);
    if (!ExpectedProgram) {
      consumeError(ExpectedProgram.takeError());
      return llvm::make_error<CodeViewError>(
          "String table offset outside of bounds of String Table!");
    }
    W.printString("Program", *ExpectedProgram);
  }
  W.printNumber("OffsetInParent", DefRangeSubfield.OffsetInParent);
  printLocalVariableAddrRange(DefRangeSubfield.Range,
                              DefRangeSubfield.getRelocationOffset());
  printLocalVariableAddrGap(DefRangeSubfield.Gaps);
  return Error::success();
}

Error CVSymbolDumperImpl::visitKnownRecord(CVSymbol &CVR,
                                           DefRangeSym &DefRange) {
  if (ObjDelegate) {
    DebugStringTableSubsectionRef Strings = ObjDelegate->getStringTable();
    auto ExpectedProgram = Strings.getString(DefRange.Program);
    if (!ExpectedProgram) {
      consumeError(ExpectedProgram.takeError());
      return llvm::make_error<CodeViewError>(
          "String table offset outside of bounds of String Table!");
    }
    W.printString("Program", *ExpectedProgram);
  }
  printLocalVariableAddrRange(DefRange.Range, DefRange.getRelocationOffset());
  printLocalVariableAddrGap(DefRange.Gaps);
  return Error::success();
}

Error CVSymbolDumperImpl::visitKnownRecord(CVSymbol &CVR,
                                           FrameCookieSym &FrameCookie) {
  StringRef LinkageName;
  if (ObjDelegate) {
    ObjDelegate->printRelocatedField("CodeOffset",
                                     FrameCookie.getRelocationOffset(),
                                     FrameCookie.CodeOffset, &LinkageName);
  }
  W.printEnum("Register", uint16_t(FrameCookie.Register), getRegisterNames());
  W.printEnum("CookieKind", uint16_t(FrameCookie.CookieKind),
              getFrameCookieKindNames());
  W.printHex("Flags", FrameCookie.Flags);
  return Error::success();
}

Error CVSymbolDumperImpl::visitKnownRecord(CVSymbol &CVR,
                                           FrameProcSym &FrameProc) {
  W.printHex("TotalFrameBytes", FrameProc.TotalFrameBytes);
  W.printHex("PaddingFrameBytes", FrameProc.PaddingFrameBytes);
  W.printHex("OffsetToPadding", FrameProc.OffsetToPadding);
  W.printHex("BytesOfCalleeSavedRegisters",
             FrameProc.BytesOfCalleeSavedRegisters);
  W.printHex("OffsetOfExceptionHandler", FrameProc.OffsetOfExceptionHandler);
  W.printHex("SectionIdOfExceptionHandler",
             FrameProc.SectionIdOfExceptionHandler);
  W.printFlags("Flags", static_cast<uint32_t>(FrameProc.Flags),
               getFrameProcSymFlagNames());
  return Error::success();
}

Error CVSymbolDumperImpl::visitKnownRecord(
    CVSymbol &CVR, HeapAllocationSiteSym &HeapAllocSite) {
  StringRef LinkageName;
  if (ObjDelegate) {
    ObjDelegate->printRelocatedField("CodeOffset",
                                     HeapAllocSite.getRelocationOffset(),
                                     HeapAllocSite.CodeOffset, &LinkageName);
  }
  W.printHex("Segment", HeapAllocSite.Segment);
  W.printHex("CallInstructionSize", HeapAllocSite.CallInstructionSize);
  printTypeIndex("Type", HeapAllocSite.Type);
  if (!LinkageName.empty())
    W.printString("LinkageName", LinkageName);
  return Error::success();
}

Error CVSymbolDumperImpl::visitKnownRecord(CVSymbol &CVR,
                                           InlineSiteSym &InlineSite) {
  W.printHex("PtrParent", InlineSite.Parent);
  W.printHex("PtrEnd", InlineSite.End);
  printTypeIndex("Inlinee", InlineSite.Inlinee);

  ListScope BinaryAnnotations(W, "BinaryAnnotations");
  for (auto &Annotation : InlineSite.annotations()) {
    switch (Annotation.OpCode) {
    case BinaryAnnotationsOpCode::Invalid:
      W.printString("(Annotation Padding)");
      break;
    case BinaryAnnotationsOpCode::CodeOffset:
    case BinaryAnnotationsOpCode::ChangeCodeOffset:
    case BinaryAnnotationsOpCode::ChangeCodeLength:
      W.printHex(Annotation.Name, Annotation.U1);
      break;
    case BinaryAnnotationsOpCode::ChangeCodeOffsetBase:
    case BinaryAnnotationsOpCode::ChangeLineEndDelta:
    case BinaryAnnotationsOpCode::ChangeRangeKind:
    case BinaryAnnotationsOpCode::ChangeColumnStart:
    case BinaryAnnotationsOpCode::ChangeColumnEnd:
      W.printNumber(Annotation.Name, Annotation.U1);
      break;
    case BinaryAnnotationsOpCode::ChangeLineOffset:
    case BinaryAnnotationsOpCode::ChangeColumnEndDelta:
      W.printNumber(Annotation.Name, Annotation.S1);
      break;
    case BinaryAnnotationsOpCode::ChangeFile:
      if (ObjDelegate) {
        W.printHex("ChangeFile",
                   ObjDelegate->getFileNameForFileOffset(Annotation.U1),
                   Annotation.U1);
      } else {
        W.printHex("ChangeFile", Annotation.U1);
      }

      break;
    case BinaryAnnotationsOpCode::ChangeCodeOffsetAndLineOffset: {
      W.startLine() << "ChangeCodeOffsetAndLineOffset: {CodeOffset: "
                    << W.hex(Annotation.U1) << ", LineOffset: " << Annotation.S1
                    << "}\n";
      break;
    }
    case BinaryAnnotationsOpCode::ChangeCodeLengthAndCodeOffset: {
      W.startLine() << "ChangeCodeLengthAndCodeOffset: {CodeOffset: "
                    << W.hex(Annotation.U2)
                    << ", Length: " << W.hex(Annotation.U1) << "}\n";
      break;
    }
    }
  }
  return Error::success();
}

Error CVSymbolDumperImpl::visitKnownRecord(CVSymbol &CVR,
                                           RegisterSym &Register) {
  printTypeIndex("Type", Register.Index);
  W.printEnum("Seg", uint16_t(Register.Register), getRegisterNames());
  W.printString("Name", Register.Name);
  return Error::success();
}

Error CVSymbolDumperImpl::visitKnownRecord(CVSymbol &CVR, PublicSym32 &Public) {
  W.printFlags("Flags", uint32_t(Public.Flags), getPublicSymFlagNames());
  W.printNumber("Seg", Public.Segment);
  W.printNumber("Off", Public.Offset);
  W.printString("Name", Public.Name);
  return Error::success();
}

Error CVSymbolDumperImpl::visitKnownRecord(CVSymbol &CVR, ProcRefSym &ProcRef) {
  W.printNumber("SumName", ProcRef.SumName);
  W.printNumber("SymOffset", ProcRef.SymOffset);
  W.printNumber("Mod", ProcRef.Module);
  W.printString("Name", ProcRef.Name);
  return Error::success();
}

Error CVSymbolDumperImpl::visitKnownRecord(CVSymbol &CVR, LabelSym &Label) {
  StringRef LinkageName;
  if (ObjDelegate) {
    ObjDelegate->printRelocatedField("CodeOffset", Label.getRelocationOffset(),
                                     Label.CodeOffset, &LinkageName);
  }
  W.printHex("Segment", Label.Segment);
  W.printHex("Flags", uint8_t(Label.Flags));
  W.printFlags("Flags", uint8_t(Label.Flags), getProcSymFlagNames());
  W.printString("DisplayName", Label.Name);
  if (!LinkageName.empty())
    W.printString("LinkageName", LinkageName);
  return Error::success();
}

Error CVSymbolDumperImpl::visitKnownRecord(CVSymbol &CVR, LocalSym &Local) {
  printTypeIndex("Type", Local.Type);
  W.printFlags("Flags", uint16_t(Local.Flags), getLocalFlagNames());
  W.printString("VarName", Local.Name);
  return Error::success();
}

Error CVSymbolDumperImpl::visitKnownRecord(CVSymbol &CVR, ObjNameSym &ObjName) {
  W.printHex("Signature", ObjName.Signature);
  W.printString("ObjectName", ObjName.Name);
  return Error::success();
}

Error CVSymbolDumperImpl::visitKnownRecord(CVSymbol &CVR, ProcSym &Proc) {
  if (InFunctionScope)
    return llvm::make_error<CodeViewError>(
        "Visiting a ProcSym while inside function scope!");

  InFunctionScope = true;

  StringRef LinkageName;
  W.printHex("PtrParent", Proc.Parent);
  W.printHex("PtrEnd", Proc.End);
  W.printHex("PtrNext", Proc.Next);
  W.printHex("CodeSize", Proc.CodeSize);
  W.printHex("DbgStart", Proc.DbgStart);
  W.printHex("DbgEnd", Proc.DbgEnd);
  printTypeIndex("FunctionType", Proc.FunctionType);
  if (ObjDelegate) {
    ObjDelegate->printRelocatedField("CodeOffset", Proc.getRelocationOffset(),
                                     Proc.CodeOffset, &LinkageName);
  }
  W.printHex("Segment", Proc.Segment);
  W.printFlags("Flags", static_cast<uint8_t>(Proc.Flags),
               getProcSymFlagNames());
  W.printString("DisplayName", Proc.Name);
  if (!LinkageName.empty())
    W.printString("LinkageName", LinkageName);
  return Error::success();
}

Error CVSymbolDumperImpl::visitKnownRecord(CVSymbol &CVR,
                                           ScopeEndSym &ScopeEnd) {
  InFunctionScope = false;
  return Error::success();
}

Error CVSymbolDumperImpl::visitKnownRecord(CVSymbol &CVR, CallerSym &Caller) {
  ListScope S(W, CVR.kind() == S_CALLEES ? "Callees" : "Callers");
  for (auto FuncID : Caller.Indices)
    printTypeIndex("FuncID", FuncID);
  return Error::success();
}

Error CVSymbolDumperImpl::visitKnownRecord(CVSymbol &CVR,
                                           RegRelativeSym &RegRel) {
  W.printHex("Offset", RegRel.Offset);
  printTypeIndex("Type", RegRel.Type);
  W.printEnum("Register", uint16_t(RegRel.Register), getRegisterNames());
  W.printString("VarName", RegRel.Name);
  return Error::success();
}

Error CVSymbolDumperImpl::visitKnownRecord(CVSymbol &CVR,
                                           ThreadLocalDataSym &Data) {
  StringRef LinkageName;
  if (ObjDelegate) {
    ObjDelegate->printRelocatedField("DataOffset", Data.getRelocationOffset(),
                                     Data.DataOffset, &LinkageName);
  }
  printTypeIndex("Type", Data.Type);
  W.printString("DisplayName", Data.Name);
  if (!LinkageName.empty())
    W.printString("LinkageName", LinkageName);
  return Error::success();
}

Error CVSymbolDumperImpl::visitKnownRecord(CVSymbol &CVR, UDTSym &UDT) {
  printTypeIndex("Type", UDT.Type);
  W.printString("UDTName", UDT.Name);
  return Error::success();
}

Error CVSymbolDumperImpl::visitUnknownSymbol(CVSymbol &CVR) {
  W.printNumber("Length", CVR.length());
  return Error::success();
}

Error CVSymbolDumper::dump(CVRecord<SymbolKind> &Record) {
  SymbolVisitorCallbackPipeline Pipeline;
  SymbolDeserializer Deserializer(ObjDelegate.get(), Container);
  CVSymbolDumperImpl Dumper(Types, ObjDelegate.get(), W, PrintRecordBytes);

  Pipeline.addCallbackToPipeline(Deserializer);
  Pipeline.addCallbackToPipeline(Dumper);
  CVSymbolVisitor Visitor(Pipeline);
  return Visitor.visitSymbolRecord(Record);
}

Error CVSymbolDumper::dump(const CVSymbolArray &Symbols) {
  SymbolVisitorCallbackPipeline Pipeline;
  SymbolDeserializer Deserializer(ObjDelegate.get(), Container);
  CVSymbolDumperImpl Dumper(Types, ObjDelegate.get(), W, PrintRecordBytes);

  Pipeline.addCallbackToPipeline(Deserializer);
  Pipeline.addCallbackToPipeline(Dumper);
  CVSymbolVisitor Visitor(Pipeline);
  return Visitor.visitSymbolStream(Symbols);
}

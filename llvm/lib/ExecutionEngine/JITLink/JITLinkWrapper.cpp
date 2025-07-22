#include "/home/felix/Github/llvm-project/llvm/tools/llvm-jitlink/llvm-jitlink.h"
#include "llvm/BinaryFormat/Magic.h"
#include "llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h"
#include "llvm/ExecutionEngine/Orc/MapperJITLinkMemoryManager.h"
#include "llvm/ExecutionEngine/Orc/SelfExecutorProcessControl.h"
#include "llvm/ExecutionEngine/Orc/AbsoluteSymbols.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/TargetSelect.h"

#include <cstring>
#include <deque>
#include <string>

#define DEBUG_TYPE "llvm_jitlink"

using namespace llvm;
using namespace llvm::jitlink;
using namespace llvm::orc;

static cl::OptionCategory JITLinkCategory("JITLink Options");

static cl::list<std::string> InputFiles(cl::Positional, cl::OneOrMore,
                                        cl::desc("input files"),
                                        cl::cat(JITLinkCategory));

static cl::opt<std::string>
    EntryPointName("entry", cl::desc("Symbol to call as main entry point"),
                   cl::init(""), cl::cat(JITLinkCategory));

static cl::list<std::string> InputArgv("args", cl::Positional,
                                       cl::desc("<program arguments>..."),
                                       cl::PositionalEatsArgs,
                                       cl::cat(JITLinkCategory));

static ExitOnError ExitOnErr;

class FunctionSymbolPlugin : public llvm::orc::ObjectLinkingLayer::Plugin {
public:
  // Store function names and addresses
  std::vector<std::pair<std::string, uint64_t>> FunctionSymbols;

  void modifyPassConfig(llvm::orc::MaterializationResponsibility &MR,
                        llvm::jitlink::LinkGraph &G,
                        llvm::jitlink::PassConfiguration &Config) override {
    for (auto *Sym : G.defined_symbols())
      if (Sym->hasName() && Sym->isCallable() && (*Sym->getName()).starts_with("func_"))
        FunctionSymbols.emplace_back((*Sym->getName()).str(), Sym->getAddress().getValue());
  }

  // Mandatory overrides (no-op if unused)
  llvm::Error notifyFailed(llvm::orc::MaterializationResponsibility &MR) override {
    return llvm::Error::success();
  }
  llvm::Error notifyRemovingResources(llvm::orc::JITDylib &JD, llvm::orc::ResourceKey K) override {
    return llvm::Error::success();
  }
  void notifyTransferringResources(llvm::orc::JITDylib &JD, llvm::orc::ResourceKey DstKey, llvm::orc::ResourceKey SrcKey) override {}
};

namespace {

template <typename ErrT>

class ConditionalPrintErr {
public:
  ConditionalPrintErr(bool C) : C(C) {}
  void operator()(ErrT &EI) {
    if (C) {
      errs() << "llvm-jitlink error: ";
      EI.log(errs());
      errs() << "\n";
    }
  }

private:
  bool C;
};

Expected<std::unique_ptr<MemoryBuffer>> getFile(const Twine &FileName) {
  if (auto F = MemoryBuffer::getFile(FileName))
    return std::move(*F);
  else
    return createFileError(FileName, F.getError());
}

void reportLLVMJITLinkError(Error Err) {
  handleAllErrors(
      std::move(Err),
      ConditionalPrintErr<orc::FailedToMaterialize>(false),
      ConditionalPrintErr<ErrorInfoBase>(true));
}

} // end anonymous namespace

namespace llvm {

static std::unique_ptr<JITLinkMemoryManager> createInProcessMemoryManager() {
  uint64_t SlabSize;
#ifdef _WIN32
  SlabSize = 1024 * 1024;
#else
  SlabSize = 1024 * 1024 * 1024;
#endif

  // Otherwise use the standard in-process mapper.
  return ExitOnErr(
      MapperJITLinkMemoryManager::CreateWithMapper<InProcessMemoryMapper>(
          SlabSize));
}

Expected<std::unique_ptr<Session>> Session::Create(Triple TT,
                                                   SubtargetFeatures Features) {

  std::unique_ptr<ExecutorProcessControl> EPC;
  /// Otherwise use SelfExecutorProcessControl to target the current process.
  auto PageSize = sys::Process::getPageSize();
  if (!PageSize)
    return PageSize.takeError();
  EPC = std::make_unique<SelfExecutorProcessControl>(
      std::make_shared<SymbolStringPool>(),
      std::make_unique<InPlaceTaskDispatcher>(), std::move(TT), *PageSize,
      createInProcessMemoryManager());

  Error Err = Error::success();
  std::unique_ptr<Session> S(new Session(std::move(EPC), Err));
  if (Err)
    return std::move(Err);
  S->Features = std::move(Features);
  return std::move(S);
}

Session::~Session() {
  if (auto Err = ES.endSession())
    ES.reportError(std::move(Err));
}

Session::Session(std::unique_ptr<ExecutorProcessControl> EPC, Error &Err)
    : ES(std::move(EPC)),
      ObjLayer(ES, ES.getExecutorProcessControl().getMemMgr()) {

  ErrorAsOutParameter _(&Err);

  ES.setErrorReporter(reportLLVMJITLinkError);

  if (auto MainJDOrErr = ES.createJITDylib("main"))
    MainJD = &*MainJDOrErr;
  else {
    Err = MainJDOrErr.takeError();
    return;
  }
}

} // end namespace llvm

static std::pair<Triple, SubtargetFeatures> getFirstFileTripleAndFeatures() {
  static std::pair<Triple, SubtargetFeatures> FirstTTAndFeatures = []() {
    assert(!InputFiles.empty() && "InputFiles can not be empty");
    for (auto InputFile : InputFiles) {
      auto ObjBuffer = ExitOnErr(getFile(InputFile));
      file_magic Magic = identify_magic(ObjBuffer->getBuffer());
      switch (Magic) {
      case file_magic::elf_relocatable: {
        auto Obj = ExitOnErr(
            object::ObjectFile::createObjectFile(ObjBuffer->getMemBufferRef()));
        Triple TT = Obj->makeTriple();
        if (Magic == file_magic::coff_object) {
          // TODO: Move this to makeTriple() if possible.
          TT.setObjectFormat(Triple::COFF);
          TT.setOS(Triple::OSType::Win32);
        }
        SubtargetFeatures Features;
        if (auto ObjFeatures = Obj->getFeatures())
          Features = std::move(*ObjFeatures);
        return std::make_pair(TT, Features);
      }
      default:
        break;
      }
    }
    return std::make_pair(Triple(), SubtargetFeatures());
  }();

  return FirstTTAndFeatures;
}

static Error sanitizeArguments(const Triple &TT, const char *ArgV0) {

  // Set the entry point name if not specified.
  if (EntryPointName.empty())
    EntryPointName = TT.getObjectFormat() == Triple::MachO ? "_main" : "main";

  return Error::success();
}

static Error createJITDylibs(Session &S,
                             std::map<unsigned, JITDylib *> &IdxToJD,
                             uint64_t StartCode, uint64_t End,
                             void *HelperFuncs, size_t HelperFuncsSize) {
  // First, set up JITDylibs.
  LLVM_DEBUG(dbgs() << "Creating JITDylibs...\n");
  // Create a "main" JITLinkDylib.
  IdxToJD[0] = S.MainJD;
  S.JDSearchOrder.push_back({S.MainJD, JITDylibLookupFlags::MatchAllSymbols});
  LLVM_DEBUG(dbgs() << "  0: " << S.MainJD->getName() << "\n");

  for (uint64_t Instr = StartCode; Instr < End; ++Instr) {
    auto VarAddr = llvm::orc::ExecutorAddr::fromPtr((uint64_t *)Instr);
    char RipOffsetHex[64] = {0};
    sprintf(RipOffsetHex, "RIP_OFFSET_0x%lx", (Instr - StartCode));
    LLVM_DEBUG(dbgs() << RipOffsetHex << " " << formatv("{0:x16}", Instr) << "\n");
    ExitOnErr(S.MainJD->define(absoluteSymbols({{S.ES.intern(RipOffsetHex), {VarAddr, JITSymbolFlags::Exported}}})));
  }

  typedef struct helper_func {
    const char *name;
    uint64_t addr;
  } helper_func_t;
  helper_func_t *Ptr = (helper_func_t *)HelperFuncs;
  for (size_t I = 0; I < (HelperFuncsSize/sizeof(helper_func_t)); ++I) {
    auto VarAddr = llvm::orc::ExecutorAddr::fromPtr((uint64_t *)Ptr[I].addr);
    ExitOnErr(S.MainJD->define(absoluteSymbols({{S.ES.intern(Ptr[I].name), {VarAddr, JITSymbolFlags::Exported}}})));
  }

  LLVM_DEBUG({
    dbgs() << "Dylib search order is [ ";
    for (auto &KV : S.JDSearchOrder)
      dbgs() << KV.first->getName() << " ";
    dbgs() << "]\n";
  });

  return Error::success();
}

static Error addObjects(Session &S,
                        const std::map<unsigned, JITDylib *> &IdxToJD) {

  // Load each object into the corresponding JITDylib..
  LLVM_DEBUG(dbgs() << "Adding objects...\n");
  for (auto InputFileItr = InputFiles.begin(), InputFileEnd = InputFiles.end();
       InputFileItr != InputFileEnd; ++InputFileItr) {
    unsigned InputFileArgIdx =
        InputFiles.getPosition(InputFileItr - InputFiles.begin());
    const std::string &InputFile = *InputFileItr;
    if (StringRef(InputFile).ends_with(".a") ||
        StringRef(InputFile).ends_with(".lib"))
      continue;
    auto &JD = *std::prev(IdxToJD.lower_bound(InputFileArgIdx))->second;
    LLVM_DEBUG(dbgs() << "  " << InputFileArgIdx << ": \"" << InputFile
                      << "\" to " << JD.getName() << "\n";);
    auto ObjBuffer = getFile(InputFile);
    if (!ObjBuffer)
      return ObjBuffer.takeError();

    if (auto Err = S.ObjLayer.add(JD, std::move(*ObjBuffer)))
      return Err;
  }

  return Error::success();
}

static Error addSessionInputs(Session &S, uint64_t StartCode, uint64_t End,
                              void *HelperFuncs, size_t HelperFuncsSize) {
  std::map<unsigned, JITDylib *> IdxToJD;

  if (auto Err = createJITDylibs(S, IdxToJD, StartCode, End, HelperFuncs, HelperFuncsSize))
    return Err;

  if (auto Err = addObjects(S, IdxToJD))
    return Err;

  return Error::success();
}

static Expected<ExecutorSymbolDef> getMainEntryPoint(Session &S) {
  return S.ES.lookup(S.JDSearchOrder, S.ES.intern(EntryPointName));
}

static Expected<ExecutorSymbolDef> getEntryPoint(Session &S) {
  ExecutorSymbolDef EntryPoint;

  // Find the entry-point function unconditionally, since we want to force
  // it to be materialized to collect stats.
  if (auto EP = getMainEntryPoint(S))
    EntryPoint = *EP;
  else
    return EP.takeError();
  LLVM_DEBUG({
    dbgs() << "Using entry point \"" << EntryPointName
           << "\": " << formatv("{0:x16}", EntryPoint.getAddress()) << "\n";
  });

  return EntryPoint;
}

uint64_t parseFuncHex(const std::string& input) {
  const std::string prefix = "func_";
  size_t prefixPos = input.find(prefix);
  if (prefixPos == std::string::npos) {
    return 0;
  }
  std::string hexStr = input.substr(prefixPos + prefix.length());
  if (hexStr.empty())
    return 0;
  auto isInvalidChar = [](char c) {
    return !std::isxdigit(static_cast<unsigned char>(c));
  };
  if (std::any_of(hexStr.begin(), hexStr.end(), isInvalidChar)) {
    return 0;
  }
  return std::stoull(hexStr, nullptr, 16);
}

extern "C" {
void *invoke_jitlink(const char *AotFile, uint64_t StartCode, uint64_t End,
                     void (*register_mapping)(uint64_t, uint64_t), void *HelperFuncs,
                     size_t HelperFuncsSize)
{
  int argc = 3;
  const char *argv[3] = {"llvm-jitlink", "--entry=main", AotFile};
  char **argv_convert = (char **)argv;
  InitLLVM X(argc, argv_convert);

  InitializeAllTargetInfos();
  InitializeAllTargetMCs();
  InitializeAllDisassemblers();

  cl::ParseCommandLineOptions(argc, argv, "llvm jitlink tool");
  ExitOnErr.setBanner(std::string(argv[0]) + ": ");
  auto [TT, Features] = getFirstFileTripleAndFeatures();
  ExitOnErr(sanitizeArguments(TT, argv[0]));
  auto S = ExitOnErr(Session::Create(TT, Features));
  ExitOnErr(addSessionInputs(*S, StartCode, End, HelperFuncs, HelperFuncsSize));

  auto Plugin = std::make_unique<FunctionSymbolPlugin>();
  auto &PluginRef = *Plugin;
  S->ObjLayer.addPlugin(std::move(Plugin));

  Expected<ExecutorSymbolDef> EntryPoint = getEntryPoint(*S);
  if (!EntryPoint) {
    reportLLVMJITLinkError(EntryPoint.takeError());
    exit(1);
  }

  for (const auto &[Name, Address] : PluginRef.FunctionSymbols) {
    Expected<ExecutorSymbolDef> Sym = S->ES.lookup(S->JDSearchOrder, S->ES.intern(Name));
    if (!Sym) {
      reportLLVMJITLinkError(EntryPoint.takeError());
      exit(1);
    }
    register_mapping((StartCode + parseFuncHex(Name)), Sym->getAddress().getValue());
  }

  return static_cast<void *>(S.release());
}
}

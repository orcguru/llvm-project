#include "/home/felix/Github/llvm-project/llvm/tools/llvm-jitlink/llvm-jitlink.h"

#include "llvm/BinaryFormat/Magic.h"
#include "llvm/ExecutionEngine/Orc/COFFPlatform.h"
#include "llvm/ExecutionEngine/Orc/COFFVCRuntimeSupport.h"
#include "llvm/ExecutionEngine/Orc/DebugObjectManagerPlugin.h"
#include "llvm/ExecutionEngine/Orc/Debugging/DebugInfoSupport.h"
#include "llvm/ExecutionEngine/Orc/Debugging/DebuggerSupportPlugin.h"
#include "llvm/ExecutionEngine/Orc/Debugging/PerfSupportPlugin.h"
#include "llvm/ExecutionEngine/Orc/Debugging/VTuneSupportPlugin.h"
#include "llvm/ExecutionEngine/Orc/ELFNixPlatform.h"
#include "llvm/ExecutionEngine/Orc/EPCDebugObjectRegistrar.h"
#include "llvm/ExecutionEngine/Orc/EPCDynamicLibrarySearchGenerator.h"
#include "llvm/ExecutionEngine/Orc/EPCEHFrameRegistrar.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/IndirectionUtils.h"
#include "llvm/ExecutionEngine/Orc/MachOPlatform.h"
#include "llvm/ExecutionEngine/Orc/MapperJITLinkMemoryManager.h"
#include "llvm/ExecutionEngine/Orc/ObjectFileInterface.h"
#include "llvm/ExecutionEngine/Orc/Shared/OrcRTBridge.h"
#include "llvm/ExecutionEngine/Orc/TargetProcess/JITLoaderGDB.h"
#include "llvm/ExecutionEngine/Orc/TargetProcess/JITLoaderPerf.h"
#include "llvm/ExecutionEngine/Orc/TargetProcess/JITLoaderVTune.h"
#include "llvm/ExecutionEngine/Orc/TargetProcess/RegisterEHFrames.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDisassembler/MCDisassembler.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCInstrAnalysis.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCTargetOptions.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Object/COFF.h"
#include "llvm/Object/MachO.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/Timer.h"

#include <cstring>
#include <deque>
#include <string>

#ifdef LLVM_ON_UNIX
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif // LLVM_ON_UNIX

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

static bool UseTestResultOverride = false;
static int64_t TestResultOverride = 0;

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

static raw_ostream &
operator<<(raw_ostream &OS, const Session::MemoryRegionInfo &MRI) {
  return OS << "target addr = "
            << format("0x%016" PRIx64, MRI.getTargetAddress())
            << ", content: " << (const void *)MRI.getContent().data() << " -- "
            << (const void *)(MRI.getContent().data() + MRI.getContent().size())
            << " (" << MRI.getContent().size() << " bytes)";
}

static raw_ostream &
operator<<(raw_ostream &OS, const Session::SymbolInfoMap &SIM) {
  OS << "Symbols:\n";
  for (auto &SKV : SIM)
    OS << "  \"" << SKV.first() << "\" " << SKV.second << "\n";
  return OS;
}

static raw_ostream &
operator<<(raw_ostream &OS, const Session::FileInfo &FI) {
  for (auto &SIKV : FI.SectionInfos)
    OS << "  Section \"" << SIKV.first() << "\": " << SIKV.second << "\n";
  for (auto &GOTKV : FI.GOTEntryInfos)
    OS << "  GOT \"" << GOTKV.first() << "\": " << GOTKV.second << "\n";
  for (auto &StubKVs : FI.StubInfos) {
    OS << "  Stubs \"" << StubKVs.first() << "\":";
    for (auto MemRegion : StubKVs.second)
      OS << " " << MemRegion;
    OS << "\n";
  }
  return OS;
}

static raw_ostream &
operator<<(raw_ostream &OS, const Session::FileInfoMap &FIM) {
  for (auto &FIKV : FIM)
    OS << "File \"" << FIKV.first() << "\":\n" << FIKV.second;
  return OS;
}

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

  auto &TT = ES.getTargetTriple();

  if (TT.isOSBinFormatELF()) {
    ObjLayer.addPlugin(std::make_unique<EHFrameRegistrationPlugin>(
        ES, ExitOnErr(EPCEHFrameRegistrar::Create(this->ES))));
  }

  if (auto MainJDOrErr = ES.createJITDylib("main"))
    MainJD = &*MainJDOrErr;
  else {
    Err = MainJDOrErr.takeError();
    return;
  }

  // If a name is defined by some harness file then it's a definition, not an
  // external.
  for (auto &DefName : HarnessDefinitions)
    HarnessExternals.erase(DefName.getKey());
}

void Session::dumpSessionInfo(raw_ostream &OS) {
  OS << "Registered addresses:\n" << SymbolInfos << FileInfos;
}

Error Session::FileInfo::registerGOTEntry(
    LinkGraph &G, Symbol &Sym, GetSymbolTargetFunction GetSymbolTarget) {
  if (Sym.isSymbolZeroFill())
    return make_error<StringError>("Unexpected zero-fill symbol in section " +
                                       Sym.getBlock().getSection().getName(),
                                   inconvertibleErrorCode());
  auto TS = GetSymbolTarget(G, Sym.getBlock());
  if (!TS)
    return TS.takeError();
  GOTEntryInfos[TS->getName()] = {Sym.getSymbolContent(),
                                  Sym.getAddress().getValue(),
                                  Sym.getTargetFlags()};
  return Error::success();
}

Error Session::FileInfo::registerStubEntry(
    LinkGraph &G, Symbol &Sym, GetSymbolTargetFunction GetSymbolTarget) {
  if (Sym.isSymbolZeroFill())
    return make_error<StringError>("Unexpected zero-fill symbol in section " +
                                       Sym.getBlock().getSection().getName(),
                                   inconvertibleErrorCode());
  auto TS = GetSymbolTarget(G, Sym.getBlock());
  if (!TS)
    return TS.takeError();

  SmallVectorImpl<MemoryRegionInfo> &Entry = StubInfos[TS->getName()];
  Entry.insert(Entry.begin(),
               {Sym.getSymbolContent(), Sym.getAddress().getValue(),
                Sym.getTargetFlags()});
  return Error::success();
}

Error Session::FileInfo::registerMultiStubEntry(
    LinkGraph &G, Symbol &Sym, GetSymbolTargetFunction GetSymbolTarget) {
  if (Sym.isSymbolZeroFill())
    return make_error<StringError>("Unexpected zero-fill symbol in section " +
                                       Sym.getBlock().getSection().getName(),
                                   inconvertibleErrorCode());

  auto Target = GetSymbolTarget(G, Sym.getBlock());
  if (!Target)
    return Target.takeError();

  SmallVectorImpl<MemoryRegionInfo> &Entry = StubInfos[Target->getName()];
  Entry.emplace_back(Sym.getSymbolContent(), Sym.getAddress().getValue(),
                     Sym.getTargetFlags());

  // Let's keep stubs ordered by ascending address.
  std::sort(Entry.begin(), Entry.end(),
            [](const MemoryRegionInfo &L, const MemoryRegionInfo &R) {
              return L.getTargetAddress() < R.getTargetAddress();
            });

  return Error::success();
}

Expected<Session::FileInfo &> Session::findFileInfo(StringRef FileName) {
  auto FileInfoItr = FileInfos.find(FileName);
  if (FileInfoItr == FileInfos.end())
    return make_error<StringError>("file \"" + FileName + "\" not recognized",
                                   inconvertibleErrorCode());
  return FileInfoItr->second;
}

Expected<Session::MemoryRegionInfo &>
Session::findSectionInfo(StringRef FileName, StringRef SectionName) {
  auto FI = findFileInfo(FileName);
  if (!FI)
    return FI.takeError();
  auto SecInfoItr = FI->SectionInfos.find(SectionName);
  if (SecInfoItr == FI->SectionInfos.end())
    return make_error<StringError>("no section \"" + SectionName +
                                       "\" registered for file \"" + FileName +
                                       "\"",
                                   inconvertibleErrorCode());
  return SecInfoItr->second;
}

class MemoryMatcher {
public:
  MemoryMatcher(ArrayRef<char> Content)
      : Pos(Content.data()), End(Pos + Content.size()) {}

  template <typename MaskType> bool matchMask(MaskType Mask) {
    if (Mask == (Mask & *reinterpret_cast<const MaskType *>(Pos))) {
      Pos += sizeof(MaskType);
      return true;
    }
    return false;
  }

  template <typename ValueType> bool matchEqual(ValueType Value) {
    if (Value == *reinterpret_cast<const ValueType *>(Pos)) {
      Pos += sizeof(ValueType);
      return true;
    }
    return false;
  }

  bool done() const { return Pos == End; }

private:
  const char *Pos;
  const char *End;
};

static StringRef detectStubKind(const Session::MemoryRegionInfo &Stub) {
  using namespace support::endian;
  auto Armv7MovWTle = byte_swap<uint32_t, endianness::little>(0xe300c000);
  auto Armv7BxR12le = byte_swap<uint32_t, endianness::little>(0xe12fff1c);
  auto Thumbv7MovWTle = byte_swap<uint32_t, endianness::little>(0x0c00f240);
  auto Thumbv7BxR12le = byte_swap<uint16_t, endianness::little>(0x4760);

  MemoryMatcher M(Stub.getContent());
  if (M.matchMask(Thumbv7MovWTle)) {
    if (M.matchMask(Thumbv7MovWTle))
      if (M.matchEqual(Thumbv7BxR12le))
        if (M.done())
          return "thumbv7_abs_le";
  } else if (M.matchMask(Armv7MovWTle)) {
    if (M.matchMask(Armv7MovWTle))
      if (M.matchEqual(Armv7BxR12le))
        if (M.done())
          return "armv7_abs_le";
  }
  return "";
}

Expected<Session::MemoryRegionInfo &>
Session::findStubInfo(StringRef FileName, StringRef TargetName,
                      StringRef KindNameFilter) {
  auto FI = findFileInfo(FileName);
  if (!FI)
    return FI.takeError();
  auto StubInfoItr = FI->StubInfos.find(TargetName);
  if (StubInfoItr == FI->StubInfos.end())
    return make_error<StringError>("no stub for \"" + TargetName +
                                       "\" registered for file \"" + FileName +
                                       "\"",
                                   inconvertibleErrorCode());
  auto &StubsForTarget = StubInfoItr->second;
  assert(!StubsForTarget.empty() && "At least 1 stub in each entry");
  if (KindNameFilter.empty() && StubsForTarget.size() == 1)
    return StubsForTarget[0]; // Regular single-stub match

  std::string KindsStr;
  SmallVector<MemoryRegionInfo *, 1> Matches;
  Regex KindNameMatcher(KindNameFilter.empty() ? ".*" : KindNameFilter);
  for (MemoryRegionInfo &Stub : StubsForTarget) {
    StringRef Kind = detectStubKind(Stub);
    if (KindNameMatcher.match(Kind))
      Matches.push_back(&Stub);
    KindsStr += "\"" + (Kind.empty() ? "<unknown>" : Kind.str()) + "\", ";
  }
  if (Matches.empty())
    return make_error<StringError>(
        "\"" + TargetName + "\" has " + Twine(StubsForTarget.size()) +
            " stubs in file \"" + FileName +
            "\", but none of them matches the stub-kind filter \"" +
            KindNameFilter + "\" (all encountered kinds are " +
            StringRef(KindsStr.data(), KindsStr.size() - 2) + ").",
        inconvertibleErrorCode());
  if (Matches.size() > 1)
    return make_error<StringError>(
        "\"" + TargetName + "\" has " + Twine(Matches.size()) +
            " candidate stubs in file \"" + FileName +
            "\". Please refine stub-kind filter \"" + KindNameFilter +
            "\" for disambiguation (encountered kinds are " +
            StringRef(KindsStr.data(), KindsStr.size() - 2) + ").",
        inconvertibleErrorCode());

  return *Matches[0];
}

Expected<Session::MemoryRegionInfo &>
Session::findGOTEntryInfo(StringRef FileName, StringRef TargetName) {
  auto FI = findFileInfo(FileName);
  if (!FI)
    return FI.takeError();
  auto GOTInfoItr = FI->GOTEntryInfos.find(TargetName);
  if (GOTInfoItr == FI->GOTEntryInfos.end())
    return make_error<StringError>("no GOT entry for \"" + TargetName +
                                       "\" registered for file \"" + FileName +
                                       "\"",
                                   inconvertibleErrorCode());
  return GOTInfoItr->second;
}

bool Session::isSymbolRegistered(StringRef SymbolName) {
  return SymbolInfos.count(SymbolName);
}

Expected<Session::MemoryRegionInfo &>
Session::findSymbolInfo(StringRef SymbolName, Twine ErrorMsgStem) {
  auto SymInfoItr = SymbolInfos.find(SymbolName);
  if (SymInfoItr == SymbolInfos.end())
    return make_error<StringError>(ErrorMsgStem + ": symbol " + SymbolName +
                                       " not found",
                                   inconvertibleErrorCode());
  return SymInfoItr->second;
}

} // end namespace llvm

static std::pair<Triple, SubtargetFeatures> getFirstFileTripleAndFeatures() {
  static std::pair<Triple, SubtargetFeatures> FirstTTAndFeatures = []() {
    assert(!InputFiles.empty() && "InputFiles can not be empty");
    for (auto InputFile : InputFiles) {
      auto ObjBuffer = ExitOnErr(getFile(InputFile));
      file_magic Magic = identify_magic(ObjBuffer->getBuffer());
      switch (Magic) {
      case file_magic::coff_object:
      case file_magic::elf_relocatable:
      case file_magic::macho_object: {
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
                             std::map<unsigned, JITDylib *> &IdxToJD) {
  // First, set up JITDylibs.
  LLVM_DEBUG(dbgs() << "Creating JITDylibs...\n");
  {
    // Create a "main" JITLinkDylib.
    IdxToJD[0] = S.MainJD;
    S.JDSearchOrder.push_back({S.MainJD, JITDylibLookupFlags::MatchAllSymbols});
    LLVM_DEBUG(dbgs() << "  0: " << S.MainJD->getName() << "\n");
  }

  if (S.PlatformJD)
    S.JDSearchOrder.push_back(
        {S.PlatformJD, JITDylibLookupFlags::MatchExportedSymbolsOnly});
  if (S.ProcessSymsJD)
    S.JDSearchOrder.push_back(
        {S.ProcessSymsJD, JITDylibLookupFlags::MatchExportedSymbolsOnly});

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

static Error addSessionInputs(Session &S) {
  std::map<unsigned, JITDylib *> IdxToJD;

  if (auto Err = createJITDylibs(S, IdxToJD))
    return Err;

  if (auto Err = addObjects(S, IdxToJD))
    return Err;

  return Error::success();
}

namespace {
struct TargetInfo {
  const Target *TheTarget;
  std::unique_ptr<MCSubtargetInfo> STI;
  std::unique_ptr<MCRegisterInfo> MRI;
  std::unique_ptr<MCAsmInfo> MAI;
  std::unique_ptr<MCContext> Ctx;
  std::unique_ptr<MCDisassembler> Disassembler;
  std::unique_ptr<MCInstrInfo> MII;
  std::unique_ptr<MCInstrAnalysis> MIA;
  std::unique_ptr<MCInstPrinter> InstPrinter;
};
} // anonymous namespace

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

static Expected<int> runWithoutRuntime(Session &S,
                                       ExecutorAddr EntryPointAddr) {
  return S.ES.getExecutorProcessControl().runAsMain(EntryPointAddr, InputArgv);
}

namespace {
struct JITLinkTimers {
  TimerGroup JITLinkTG{"llvm-jitlink timers", "timers for llvm-jitlink phases"};
  Timer LoadObjectsTimer{"load", "time to load/add object files", JITLinkTG};
  Timer LinkTimer{"link", "time to link object files", JITLinkTG};
  Timer RunTimer{"run", "time to execute jitlink'd code", JITLinkTG};
};
} // namespace

extern "C" {
void invoke_jitlink(const char *AotFile) {
  int argc = 4;
  const char *argv[4] = {"llvm-jitlink", "--debug-only=jitlink", AotFile, "/home/felix/Github/single_thread_demo_translate/scripts/Scratch/experiment_with_jitlink/main.o"};
  //int argc = 2;
  //const char *argv[2] = {"llvm-jitlink", "--help"};
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

  {
    ExitOnErr(addSessionInputs(*S));
  }

  Expected<ExecutorSymbolDef> EntryPoint((ExecutorSymbolDef()));
  {
    ExpectedAsOutParameter<ExecutorSymbolDef> _(&EntryPoint);
    EntryPoint = getEntryPoint(*S);
  }

  if (!EntryPoint) {
    reportLLVMJITLinkError(EntryPoint.takeError());
    exit(1);
  }

  int Result = 0;
  LLVM_DEBUG(dbgs() << "Running \"" << EntryPointName << "\"...\n");
  Result = ExitOnErr(
      runWithoutRuntime(*S, ExecutorAddr(EntryPoint->getAddress())));

  // Destroy the session.
  ExitOnErr(S->ES.endSession());
  S.reset();
}
}

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

static cl::opt<bool> NoExec("noexec", cl::desc("Do not execute loaded code"),
                            cl::init(false), cl::cat(JITLinkCategory));

static cl::opt<std::string>
    EntryPointName("entry", cl::desc("Symbol to call as main entry point"),
                   cl::init(""), cl::cat(JITLinkCategory));

static cl::list<std::string> JITDylibs(
    "jd",
    cl::desc("Specifies the JITDylib to be used for any subsequent "
             "input file, -L<seacrh-path>, and -l<library> arguments"),
    cl::cat(JITLinkCategory));

static cl::list<std::string>
    Dylibs("preload",
           cl::desc("Pre-load dynamic libraries (e.g. language runtimes "
                    "required by the ORC runtime)"),
           cl::cat(JITLinkCategory));

static cl::list<std::string> InputArgv("args", cl::Positional,
                                       cl::desc("<program arguments>..."),
                                       cl::PositionalEatsArgs,
                                       cl::cat(JITLinkCategory));

static cl::opt<bool>
    NoProcessSymbols("no-process-syms",
                     cl::desc("Do not resolve to llvm-jitlink process symbols"),
                     cl::init(false), cl::cat(JITLinkCategory));

static cl::opt<std::string> SlabAllocateSizeString(
    "slab-allocate",
    cl::desc("Allocate from a slab of the given size "
             "(allowable suffixes: Kb, Mb, Gb. default = "
             "Kb)"),
    cl::init(""), cl::cat(JITLinkCategory));

static cl::opt<uint64_t> SlabAddress(
    "slab-address",
    cl::desc("Set slab target address (requires -slab-allocate and -noexec)"),
    cl::init(~0ULL), cl::cat(JITLinkCategory));

static cl::opt<uint64_t> SlabPageSize(
    "slab-page-size",
    cl::desc("Set page size for slab (requires -slab-allocate and -noexec)"),
    cl::init(0), cl::cat(JITLinkCategory));

static ExitOnError ExitOnErr;

static LLVM_ATTRIBUTE_USED void linkComponents() {
}

static bool UseTestResultOverride = false;
static int64_t TestResultOverride = 0;

extern "C" LLVM_ATTRIBUTE_USED void
llvm_jitlink_setTestResultOverride(int64_t Value) {
  TestResultOverride = Value;
  UseTestResultOverride = true;
}

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

static Error applyHarnessPromotions(Session &S, LinkGraph &G) {

  // If this graph is part of the test harness there's nothing to do.
  if (S.HarnessFiles.empty() || S.HarnessFiles.count(G.getName()))
    return Error::success();

  LLVM_DEBUG(dbgs() << "Applying promotions to graph " << G.getName() << "\n");

  // If this graph is part of the test then promote any symbols referenced by
  // the harness to default scope, remove all symbols that clash with harness
  // definitions.
  std::vector<Symbol *> DefinitionsToRemove;
  for (auto *Sym : G.defined_symbols()) {

    if (!Sym->hasName())
      continue;

    if (Sym->getLinkage() == Linkage::Weak) {
      if (!S.CanonicalWeakDefs.count(Sym->getName()) ||
          S.CanonicalWeakDefs[Sym->getName()] != G.getName()) {
        LLVM_DEBUG({
          dbgs() << "  Externalizing weak symbol " << Sym->getName() << "\n";
        });
        DefinitionsToRemove.push_back(Sym);
      } else {
        LLVM_DEBUG({
          dbgs() << "  Making weak symbol " << Sym->getName() << " strong\n";
        });
        if (S.HarnessExternals.count(Sym->getName()))
          Sym->setScope(Scope::Default);
        else
          Sym->setScope(Scope::Hidden);
        Sym->setLinkage(Linkage::Strong);
      }
    } else if (S.HarnessExternals.count(Sym->getName())) {
      LLVM_DEBUG(dbgs() << "  Promoting " << Sym->getName() << "\n");
      Sym->setScope(Scope::Default);
      Sym->setLive(true);
      continue;
    } else if (S.HarnessDefinitions.count(Sym->getName())) {
      LLVM_DEBUG(dbgs() << "  Externalizing " << Sym->getName() << "\n");
      DefinitionsToRemove.push_back(Sym);
    }
  }

  for (auto *Sym : DefinitionsToRemove)
    G.makeExternal(*Sym);

  return Error::success();
}

// A memory mapper with a fake offset applied only used for -noexec testing
class InProcessDeltaMapper final : public InProcessMemoryMapper {
public:
  InProcessDeltaMapper(size_t PageSize, uint64_t TargetAddr)
      : InProcessMemoryMapper(PageSize), TargetMapAddr(TargetAddr),
        DeltaAddr(0) {}

  static Expected<std::unique_ptr<InProcessDeltaMapper>> Create() {
    size_t PageSize = SlabPageSize;
    if (!PageSize) {
      if (auto PageSizeOrErr = sys::Process::getPageSize())
        PageSize = *PageSizeOrErr;
      else
        return PageSizeOrErr.takeError();
    }

    if (PageSize == 0)
      return make_error<StringError>("Page size is zero",
                                     inconvertibleErrorCode());

    return std::make_unique<InProcessDeltaMapper>(PageSize, SlabAddress);
  }

  void reserve(size_t NumBytes, OnReservedFunction OnReserved) override {
    InProcessMemoryMapper::reserve(
        NumBytes, [this, OnReserved = std::move(OnReserved)](
                      Expected<ExecutorAddrRange> Result) mutable {
          if (!Result)
            return OnReserved(Result.takeError());

          assert(DeltaAddr == 0 && "Overwriting previous offset");
          if (TargetMapAddr != ~0ULL)
            DeltaAddr = TargetMapAddr - Result->Start.getValue();
          auto OffsetRange = ExecutorAddrRange(Result->Start + DeltaAddr,
                                               Result->End + DeltaAddr);

          OnReserved(OffsetRange);
        });
  }

  char *prepare(ExecutorAddr Addr, size_t ContentSize) override {
    return InProcessMemoryMapper::prepare(Addr - DeltaAddr, ContentSize);
  }

  void initialize(AllocInfo &AI, OnInitializedFunction OnInitialized) override {
    // Slide mapping based on delta, make all segments read-writable, and
    // discard allocation actions.
    auto FixedAI = std::move(AI);
    FixedAI.MappingBase -= DeltaAddr;
    for (auto &Seg : FixedAI.Segments)
      Seg.AG = {MemProt::Read | MemProt::Write, Seg.AG.getMemLifetime()};
    FixedAI.Actions.clear();
    InProcessMemoryMapper::initialize(
        FixedAI, [this, OnInitialized = std::move(OnInitialized)](
                     Expected<ExecutorAddr> Result) mutable {
          if (!Result)
            return OnInitialized(Result.takeError());

          OnInitialized(ExecutorAddr(Result->getValue() + DeltaAddr));
        });
  }

  void deinitialize(ArrayRef<ExecutorAddr> Allocations,
                    OnDeinitializedFunction OnDeInitialized) override {
    std::vector<ExecutorAddr> Addrs(Allocations.size());
    for (const auto Base : Allocations) {
      Addrs.push_back(Base - DeltaAddr);
    }

    InProcessMemoryMapper::deinitialize(Addrs, std::move(OnDeInitialized));
  }

  void release(ArrayRef<ExecutorAddr> Reservations,
               OnReleasedFunction OnRelease) override {
    std::vector<ExecutorAddr> Addrs(Reservations.size());
    for (const auto Base : Reservations) {
      Addrs.push_back(Base - DeltaAddr);
    }
    InProcessMemoryMapper::release(Addrs, std::move(OnRelease));
  }

private:
  uint64_t TargetMapAddr;
  uint64_t DeltaAddr;
};

Expected<uint64_t> getSlabAllocSize(StringRef SizeString) {
  SizeString = SizeString.trim();

  uint64_t Units = 1024;

  if (SizeString.ends_with_insensitive("kb"))
    SizeString = SizeString.drop_back(2).rtrim();
  else if (SizeString.ends_with_insensitive("mb")) {
    Units = 1024 * 1024;
    SizeString = SizeString.drop_back(2).rtrim();
  } else if (SizeString.ends_with_insensitive("gb")) {
    Units = 1024 * 1024 * 1024;
    SizeString = SizeString.drop_back(2).rtrim();
  }

  uint64_t SlabSize = 0;
  if (SizeString.getAsInteger(10, SlabSize))
    return make_error<StringError>("Invalid numeric format for slab size",
                                   inconvertibleErrorCode());

  return SlabSize * Units;
}

static std::unique_ptr<JITLinkMemoryManager> createInProcessMemoryManager() {
  uint64_t SlabSize;
#ifdef _WIN32
  SlabSize = 1024 * 1024;
#else
  SlabSize = 1024 * 1024 * 1024;
#endif

  if (!SlabAllocateSizeString.empty())
    SlabSize = ExitOnErr(getSlabAllocSize(SlabAllocateSizeString));

  // If this is a -no-exec case and we're tweaking the slab address or size then
  // use the delta mapper.
  if (NoExec && (SlabAddress || SlabPageSize))
    return ExitOnErr(
        MapperJITLinkMemoryManager::CreateWithMapper<InProcessDeltaMapper>(
            SlabSize));

  // Otherwise use the standard in-process mapper.
  return ExitOnErr(
      MapperJITLinkMemoryManager::CreateWithMapper<InProcessMemoryMapper>(
          SlabSize));
}

Expected<std::unique_ptr<jitlink::JITLinkMemoryManager>>
createSharedMemoryManager(SimpleRemoteEPC &SREPC) {
  SharedMemoryMapper::SymbolAddrs SAs;
  if (auto Err = SREPC.getBootstrapSymbols(
          {{SAs.Instance, rt::ExecutorSharedMemoryMapperServiceInstanceName},
           {SAs.Reserve,
            rt::ExecutorSharedMemoryMapperServiceReserveWrapperName},
           {SAs.Initialize,
            rt::ExecutorSharedMemoryMapperServiceInitializeWrapperName},
           {SAs.Deinitialize,
            rt::ExecutorSharedMemoryMapperServiceDeinitializeWrapperName},
           {SAs.Release,
            rt::ExecutorSharedMemoryMapperServiceReleaseWrapperName}}))
    return std::move(Err);

#ifdef _WIN32
  size_t SlabSize = 1024 * 1024;
#else
  size_t SlabSize = 1024 * 1024 * 1024;
#endif

  if (!SlabAllocateSizeString.empty())
    SlabSize = ExitOnErr(getSlabAllocSize(SlabAllocateSizeString));

  return MapperJITLinkMemoryManager::CreateWithMapper<SharedMemoryMapper>(
      SlabSize, SREPC, SAs);
}


static Expected<MaterializationUnit::Interface>
getTestObjectFileInterface(Session &S, MemoryBufferRef O) {

  // Get the standard interface for this object, but ignore the symbols field.
  // We'll handle that manually to include promotion.
  auto I = getObjectFileInterface(S.ES, O);
  if (!I)
    return I.takeError();
  I->SymbolFlags.clear();

  // If creating an object file was going to fail it would have happened above,
  // so we can 'cantFail' this.
  auto Obj = cantFail(object::ObjectFile::createObjectFile(O));

  // The init symbol must be included in the SymbolFlags map if present.
  if (I->InitSymbol)
    I->SymbolFlags[I->InitSymbol] =
        JITSymbolFlags::MaterializationSideEffectsOnly;

  for (auto &Sym : Obj->symbols()) {
    Expected<uint32_t> SymFlagsOrErr = Sym.getFlags();
    if (!SymFlagsOrErr)
      // TODO: Test this error.
      return SymFlagsOrErr.takeError();

    // Skip symbols not defined in this object file.
    if ((*SymFlagsOrErr & object::BasicSymbolRef::SF_Undefined))
      continue;

    auto Name = Sym.getName();
    if (!Name)
      return Name.takeError();

    // Skip symbols that have type SF_File.
    if (auto SymType = Sym.getType()) {
      if (*SymType == object::SymbolRef::ST_File)
        continue;
    } else
      return SymType.takeError();

    auto SymFlags = JITSymbolFlags::fromObjectSymbol(Sym);
    if (!SymFlags)
      return SymFlags.takeError();

    if (SymFlags->isWeak()) {
      // If this is a weak symbol that's not defined in the harness then we
      // need to either mark it as strong (if this is the first definition
      // that we've seen) or discard it.
      if (S.HarnessDefinitions.count(*Name) || S.CanonicalWeakDefs.count(*Name))
        continue;
      S.CanonicalWeakDefs[*Name] = O.getBufferIdentifier();
      *SymFlags &= ~JITSymbolFlags::Weak;
      if (!S.HarnessExternals.count(*Name))
        *SymFlags &= ~JITSymbolFlags::Exported;
    } else if (S.HarnessExternals.count(*Name)) {
      *SymFlags |= JITSymbolFlags::Exported;
    } else if (S.HarnessDefinitions.count(*Name) ||
               !(*SymFlagsOrErr & object::BasicSymbolRef::SF_Global))
      continue;

    auto InternedName = S.ES.intern(*Name);
    I->SymbolFlags[InternedName] = std::move(*SymFlags);
  }

  return I;
}

static Error loadProcessSymbols(Session &S) {
  S.ProcessSymsJD = &S.ES.createBareJITDylib("Process");
  auto FilterMainEntryPoint =
      [EPName = S.ES.intern(EntryPointName)](SymbolStringPtr Name) {
        return Name != EPName;
      };
  S.ProcessSymsJD->addGenerator(
      ExitOnErr(orc::EPCDynamicLibrarySearchGenerator::GetForTargetProcess(
          S.ES, std::move(FilterMainEntryPoint))));

  return Error::success();
}

static Error loadDylibs(Session &S) {
  LLVM_DEBUG(dbgs() << "Loading dylibs...\n");
  for (const auto &Dylib : Dylibs) {
    LLVM_DEBUG(dbgs() << "  " << Dylib << "\n");
    auto DL = S.getOrLoadDynamicLibrary(Dylib);
    if (!DL)
      return DL.takeError();
  }

  return Error::success();
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

  /// Local ObjectLinkingLayer::Plugin class to forward modifyPassConfig to the
  /// Session.
  class JITLinkSessionPlugin : public ObjectLinkingLayer::Plugin {
  public:
    JITLinkSessionPlugin(Session &S) : S(S) {}
    void modifyPassConfig(MaterializationResponsibility &MR, LinkGraph &G,
                          PassConfiguration &PassConfig) override {
      S.modifyPassConfig(G.getTargetTriple(), PassConfig);
    }

    Error notifyFailed(MaterializationResponsibility &MR) override {
      return Error::success();
    }
    Error notifyRemovingResources(JITDylib &JD, ResourceKey K) override {
      return Error::success();
    }
    void notifyTransferringResources(JITDylib &JD, ResourceKey DstKey,
                                     ResourceKey SrcKey) override {}

  private:
    Session &S;
  };

  ErrorAsOutParameter _(&Err);

  ES.setErrorReporter(reportLLVMJITLinkError);

  if (!NoProcessSymbols)
    ExitOnErr(loadProcessSymbols(*this));

  ExitOnErr(loadDylibs(*this));

  auto &TT = ES.getTargetTriple();

  if (TT.isOSBinFormatELF()) {
    if (!NoExec)
      ObjLayer.addPlugin(std::make_unique<EHFrameRegistrationPlugin>(
          ES, ExitOnErr(EPCEHFrameRegistrar::Create(this->ES))));
  }

  if (auto MainJDOrErr = ES.createJITDylib("main"))
    MainJD = &*MainJDOrErr;
  else {
    Err = MainJDOrErr.takeError();
    return;
  }

  if (NoProcessSymbols) {
    // This symbol is used in testcases, but we're not reflecting process
    // symbols so we'll need to make it available some other way.
    auto &TestResultJD = ES.createBareJITDylib("<TestResultJD>");
    ExitOnErr(TestResultJD.define(absoluteSymbols(
        {{ES.intern("llvm_jitlink_setTestResultOverride"),
          {ExecutorAddr::fromPtr(llvm_jitlink_setTestResultOverride),
           JITSymbolFlags::Exported}}})));
    MainJD->addToLinkOrder(TestResultJD);
  }

  ObjLayer.addPlugin(std::make_unique<JITLinkSessionPlugin>(*this));

  // If a name is defined by some harness file then it's a definition, not an
  // external.
  for (auto &DefName : HarnessDefinitions)
    HarnessExternals.erase(DefName.getKey());
}

void Session::dumpSessionInfo(raw_ostream &OS) {
  OS << "Registered addresses:\n" << SymbolInfos << FileInfos;
}

void Session::modifyPassConfig(const Triple &TT,
                               PassConfiguration &PassConfig) {
  PassConfig.PrePrunePasses.push_back(
      [this](LinkGraph &G) { return applyHarnessPromotions(*this, G); });
}

Expected<JITDylib *> Session::getOrLoadDynamicLibrary(StringRef LibPath) {
  auto It = DynLibJDs.find(LibPath.str());
  if (It != DynLibJDs.end()) {
    return It->second;
  }
  auto G = EPCDynamicLibrarySearchGenerator::Load(ES, LibPath.data());
  if (!G)
    return G.takeError();
  auto JD = &ES.createBareJITDylib(LibPath.str());

  JD->addGenerator(std::move(*G));
  DynLibJDs.emplace(LibPath.str(), JD);
  LLVM_DEBUG({
    dbgs() << "Loaded dynamic library " << LibPath.data() << " for " << LibPath
           << "\n";
  });
  return JD;
}

Error Session::loadAndLinkDynamicLibrary(JITDylib &JD, StringRef LibPath) {
  auto DL = getOrLoadDynamicLibrary(LibPath);
  if (!DL)
    return DL.takeError();
  JD.addToLinkOrder(**DL);
  LLVM_DEBUG({
    dbgs() << "Linking dynamic library " << LibPath << " to " << JD.getName()
           << "\n";
  });
  return Error::success();
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

  // -noexec and --args should not be used together.
  if (NoExec && !InputArgv.empty())
    errs() << "Warning: --args passed to -noexec run will be ignored.\n";

  // Set the entry point name if not specified.
  if (EntryPointName.empty())
    EntryPointName = TT.getObjectFormat() == Triple::MachO ? "_main" : "main";

  // If -slab-address is passed, require -slab-allocate and -noexec
  if (SlabAddress != ~0ULL) {
    if (SlabAllocateSizeString == "" || !NoExec)
      return make_error<StringError>(
          "-slab-address requires -slab-allocate and -noexec",
          inconvertibleErrorCode());

    if (SlabPageSize == 0)
      errs() << "Warning: -slab-address used without -slab-page-size.\n";
  }

  if (SlabPageSize != 0) {
    // -slab-page-size requires slab alloc.
    if (SlabAllocateSizeString == "")
      return make_error<StringError>("-slab-page-size requires -slab-allocate",
                                     inconvertibleErrorCode());

    // Check -slab-page-size / -noexec interactions.
    if (!NoExec) {
      if (auto RealPageSize = sys::Process::getPageSize()) {
        if (SlabPageSize % *RealPageSize)
          return make_error<StringError>(
              "-slab-page-size must be a multiple of real page size for exec "
              "tests (did you mean to use -noexec ?)\n",
              inconvertibleErrorCode());
      } else {
        errs() << "Could not retrieve process page size:\n";
        logAllUnhandledErrors(RealPageSize.takeError(), errs(), "");
        errs() << "Executing with slab page size = "
               << formatv("{0:x}", SlabPageSize) << ".\n"
               << "Tool may crash if " << formatv("{0:x}", SlabPageSize)
               << " is not a multiple of the real process page size.\n"
               << "(did you mean to use -noexec ?)";
      }
    }
  }

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

    // Add any extra JITDylibs from the command line.
    for (auto JDItr = JITDylibs.begin(), JDEnd = JITDylibs.end();
         JDItr != JDEnd; ++JDItr) {
      auto JD = S.ES.createJITDylib(*JDItr);
      if (!JD)
        return JD.takeError();
      unsigned JDIdx = JITDylibs.getPosition(JDItr - JITDylibs.begin());
      IdxToJD[JDIdx] = &*JD;
      S.JDSearchOrder.push_back({&*JD, JITDylibLookupFlags::MatchAllSymbols});
      LLVM_DEBUG(dbgs() << "  " << JDIdx << ": " << JD->getName() << "\n");
    }
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

    if (S.HarnessFiles.empty()) {
      if (auto Err = S.ObjLayer.add(JD, std::move(*ObjBuffer)))
        return Err;
    } else {
      // We're in -harness mode. Use a custom interface for this
      // test object.
      auto ObjInterface =
          getTestObjectFileInterface(S, (*ObjBuffer)->getMemBufferRef());
      if (!ObjInterface)
        return ObjInterface.takeError();
      if (auto Err = S.ObjLayer.add(JD, std::move(*ObjBuffer),
                                    std::move(*ObjInterface)))
        return Err;
    }
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

static bool isELFGOTSection(Section &S) { return S.getName() == "$__GOT"; }

static bool isELFStubsSection(Section &S) { return S.getName() == "$__STUBS"; }

static bool isELFAArch32StubsSection(Section &S) {
  return S.getName().starts_with("__llvm_jitlink_aarch32_STUBS_");
}

static Expected<Edge &> getFirstRelocationEdge(LinkGraph &G, Block &B) {
  auto EItr =
      llvm::find_if(B.edges(), [](Edge &E) { return E.isRelocation(); });
  if (EItr == B.edges().end())
    return make_error<StringError>("GOT entry in " + G.getName() + ", \"" +
                                       B.getSection().getName() +
                                       "\" has no relocations",
                                   inconvertibleErrorCode());
  return *EItr;
}

static Expected<Symbol &> getELFGOTTarget(LinkGraph &G, Block &B) {
  auto E = getFirstRelocationEdge(G, B);
  if (!E)
    return E.takeError();
  auto &TargetSym = E->getTarget();
  if (!TargetSym.hasName())
    return make_error<StringError>(
        "GOT entry in " + G.getName() + ", \"" +
            TargetSym.getBlock().getSection().getName() +
            "\" points to anonymous "
            "symbol",
        inconvertibleErrorCode());
  return TargetSym;
}

static Expected<Symbol &> getELFStubTarget(LinkGraph &G, Block &B) {
  auto E = getFirstRelocationEdge(G, B);
  if (!E)
    return E.takeError();
  auto &GOTSym = E->getTarget();
  if (!GOTSym.isDefined())
    return make_error<StringError>("Stubs entry in " + G.getName() +
                                       " does not point to GOT entry",
                                   inconvertibleErrorCode());
  if (!isELFGOTSection(GOTSym.getBlock().getSection()))
    return make_error<StringError>(
        "Stubs entry in " + G.getName() + ", \"" +
            GOTSym.getBlock().getSection().getName() +
            "\" does not point to GOT entry",
        inconvertibleErrorCode());
  return getELFGOTTarget(G, GOTSym.getBlock());
}

static Expected<Symbol &> getELFAArch32StubTarget(LinkGraph &G, Block &B) {
  auto E = getFirstRelocationEdge(G, B);
  if (!E)
    return E.takeError();
  return E->getTarget();
}

enum SectionType { GOT, Stubs, AArch32Stubs, Other };

static Error registerSymbol(LinkGraph &G, Symbol &Sym, Session::FileInfo &FI,
                            SectionType SecType) {
  switch (SecType) {
  case GOT:
    if (Sym.getSize() == 0)
      return Error::success(); // Skip the GOT start symbol
    return FI.registerGOTEntry(G, Sym, getELFGOTTarget);
  case Stubs:
    return FI.registerStubEntry(G, Sym, getELFStubTarget);
  case AArch32Stubs:
    return FI.registerMultiStubEntry(G, Sym, getELFAArch32StubTarget);
  case Other:
    return Error::success();
  }
  llvm_unreachable("Unhandled SectionType enum");
}

namespace llvm {

Error registerELFGraphInfo(Session &S, LinkGraph &G) {
  auto FileName = sys::path::filename(G.getName());
  if (S.FileInfos.count(FileName)) {
    return make_error<StringError>("When -check is passed, file names must be "
                                   "distinct (duplicate: \"" +
                                       FileName + "\")",
                                   inconvertibleErrorCode());
  }

  auto &FileInfo = S.FileInfos[FileName];
  LLVM_DEBUG({
    dbgs() << "Registering ELF file info for \"" << FileName << "\"\n";
  });
  for (auto &Sec : G.sections()) {
    LLVM_DEBUG({
      dbgs() << "  Section \"" << Sec.getName() << "\": "
             << (Sec.symbols().empty() ? "empty. skipping." : "processing...")
             << "\n";
    });

    // Skip empty sections.
    if (Sec.symbols().empty())
      continue;

    if (FileInfo.SectionInfos.count(Sec.getName()))
      return make_error<StringError>("Encountered duplicate section name \"" +
                                         Sec.getName() + "\" in \"" + FileName +
                                         "\"",
                                     inconvertibleErrorCode());

    SectionType SecType;
    if (isELFGOTSection(Sec)) {
      SecType = GOT;
    } else if (isELFStubsSection(Sec)) {
      SecType = Stubs;
    } else if (isELFAArch32StubsSection(Sec)) {
      SecType = AArch32Stubs;
    } else {
      SecType = Other;
    }

    bool SectionContainsContent = false;
    bool SectionContainsZeroFill = false;

    auto *FirstSym = *Sec.symbols().begin();
    auto *LastSym = FirstSym;
    for (auto *Sym : Sec.symbols()) {
      if (Sym->getAddress() < FirstSym->getAddress())
        FirstSym = Sym;
      if (Sym->getAddress() > LastSym->getAddress())
        LastSym = Sym;

      if (SecType != Other) {
        if (Error Err = registerSymbol(G, *Sym, FileInfo, SecType))
          return Err;
        SectionContainsContent = true;
      }

      if (Sym->hasName()) {
        if (Sym->isSymbolZeroFill()) {
          S.SymbolInfos[Sym->getName()] = {Sym->getSize(),
                                           Sym->getAddress().getValue()};
          SectionContainsZeroFill = true;
        } else {
          S.SymbolInfos[Sym->getName()] = {Sym->getSymbolContent(),
                                           Sym->getAddress().getValue(),
                                           Sym->getTargetFlags()};
          SectionContainsContent = true;
        }
      }
    }

    // Add symbol info for absolute symbols.
    for (auto *Sym : G.absolute_symbols())
      S.SymbolInfos[Sym->getName()] = {Sym->getSize(),
                                       Sym->getAddress().getValue()};

    auto SecAddr = FirstSym->getAddress();
    auto SecSize =
        (LastSym->getBlock().getAddress() + LastSym->getBlock().getSize()) -
        SecAddr;

    if (SectionContainsZeroFill && SectionContainsContent)
      return make_error<StringError>("Mixed zero-fill and content sections not "
                                     "supported yet",
                                     inconvertibleErrorCode());
    if (SectionContainsZeroFill)
      FileInfo.SectionInfos[Sec.getName()] = {SecSize, SecAddr.getValue()};
    else
      FileInfo.SectionInfos[Sec.getName()] = {
          ArrayRef<char>(FirstSym->getBlock().getContent().data(), SecSize),
          SecAddr.getValue(), FirstSym->getTargetFlags()};
  }

  return Error::success();
}

} // end namespace llvm

static cl::opt<bool> ShowPrePruneTotalBlockSize(
    "pre-prune-total-block-size",
    cl::desc("Total size of all blocks (including zero-fill) in all "
             "graphs (pre-pruning)"),
    cl::init(false));

static cl::opt<bool> ShowPostFixupTotalBlockSize(
    "post-fixup-total-block-size",
    cl::desc("Total size of all blocks (including zero-fill) in all "
             "graphs (post-fixup)"),
    cl::init(false));

class StatsPlugin : public ObjectLinkingLayer::Plugin {
public:
  static void enableIfNeeded(Session &S, bool UsingOrcRuntime) {
    std::unique_ptr<StatsPlugin> Instance;
    auto GetStats = [&]() -> StatsPlugin & {
      if (!Instance)
        Instance.reset(new StatsPlugin(UsingOrcRuntime));
      return *Instance;
    };

    if (ShowPrePruneTotalBlockSize)
      GetStats().PrePruneTotalBlockSize = 0;

    if (ShowPostFixupTotalBlockSize)
      GetStats().PostFixupTotalBlockSize = 0;

    if (Instance)
      S.ObjLayer.addPlugin(std::move(Instance));
  }

  ~StatsPlugin() { publish(dbgs()); }

  void publish(raw_ostream &OS);

  void modifyPassConfig(MaterializationResponsibility &MR, LinkGraph &G,
                        PassConfiguration &PassConfig) override {
    PassConfig.PrePrunePasses.push_back(
        [this](LinkGraph &G) { return recordPrePruneStats(G); });
    PassConfig.PostFixupPasses.push_back(
        [this](LinkGraph &G) { return recordPostFixupStats(G); });
  }

  Error notifyFailed(MaterializationResponsibility &MR) override {
    return Error::success();
  }

  Error notifyRemovingResources(JITDylib &JD, ResourceKey K) override {
    return Error::success();
  }

  void notifyTransferringResources(JITDylib &JD, ResourceKey DstKey,
                                   ResourceKey SrcKey) override {}

private:
  StatsPlugin(bool UsingOrcRuntime) : UsingOrcRuntime(UsingOrcRuntime) {}
  Error recordPrePruneStats(jitlink::LinkGraph &G);
  Error recordPostFixupStats(jitlink::LinkGraph &G);

  bool UsingOrcRuntime;

  std::mutex M;
  std::optional<uint64_t> PrePruneTotalBlockSize;
  std::optional<uint64_t> PostFixupTotalBlockSize;
  std::optional<DenseMap<size_t, size_t>> EdgeCountDetails;
};

void StatsPlugin::publish(raw_ostream &OS) {

  if (UsingOrcRuntime)
    OS << "Note: Session stats include runtime and entry point lookup, but "
          "not JITDylib initialization/deinitialization.\n";

  OS << "Statistics:\n";
  if (PrePruneTotalBlockSize)
    OS << "  Total size of all blocks before pruning: "
       << *PrePruneTotalBlockSize << "\n";

  if (PostFixupTotalBlockSize)
    OS << "  Total size of all blocks after fixups: "
       << *PostFixupTotalBlockSize << "\n";
}

static uint64_t computeTotalBlockSizes(LinkGraph &G) {
  uint64_t TotalSize = 0;
  for (auto *B : G.blocks())
    TotalSize += B->getSize();
  return TotalSize;
}

Error StatsPlugin::recordPrePruneStats(LinkGraph &G) {
  std::lock_guard<std::mutex> Lock(M);

  if (PrePruneTotalBlockSize)
    *PrePruneTotalBlockSize += computeTotalBlockSizes(G);

  return Error::success();
}

Error StatsPlugin::recordPostFixupStats(LinkGraph &G) {
  std::lock_guard<std::mutex> Lock(M);

  if (PostFixupTotalBlockSize)
    *PostFixupTotalBlockSize += computeTotalBlockSizes(G);
  return Error::success();
}

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
  if (!NoExec) {
    LLVM_DEBUG(dbgs() << "Running \"" << EntryPointName << "\"...\n");
    Result = ExitOnErr(
        runWithoutRuntime(*S, ExecutorAddr(EntryPoint->getAddress())));
  }

  // Destroy the session.
  ExitOnErr(S->ES.endSession());
  S.reset();
}
}

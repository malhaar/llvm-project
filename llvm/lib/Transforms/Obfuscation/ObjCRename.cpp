// For open-source license, please refer to
// [License](https://github.com/HikariObfuscator/Hikari/wiki/License).
//===----------------------------------------------------------------------===//
#include "llvm/Transforms/Obfuscation/ObjCRename.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <string>
#include <vector>

using namespace llvm;

static cl::opt<std::string>
    ObjCRenamePrefix("objc-rename-prefix", cl::init("IGP_"), cl::NotHidden,
                     cl::desc("Prefix for ObjC symbols to rename"));

namespace llvm {

struct ObjCRename : public ModulePass {
  static char ID;
  bool flag;

  // Shared rename map: original name -> hashed name
  std::map<std::string, std::string> RenameMap;

  ObjCRename() : ModulePass(ID) { this->flag = true; }
  ObjCRename(bool flag) : ModulePass(ID) { this->flag = flag; }

  StringRef getPassName() const override { return "ObjCRename"; }

  /// Hash a single component (no colons) to the same length using MD5.
  /// First char is always a letter [a-z], rest are [a-z0-9].
  std::string hashComponent(StringRef Input) {
    size_t Len = Input.size();
    if (Len == 0)
      return "";

    std::string Result;
    Result.reserve(Len);

    // We may need multiple MD5 rounds if the name is longer than 16 bytes
    unsigned Round = 0;
    while (Result.size() < Len) {
      MD5 Hash;
      Hash.update(Input);
      if (Round > 0) {
        Hash.update("_");
        Hash.update(std::to_string(Round));
      }
      MD5::MD5Result Digest;
      Hash.final(Digest);

      for (unsigned i = 0; i < 16 && Result.size() < Len; i++) {
        uint8_t Byte = Digest[i];
        if (Result.empty()) {
          // First character must be a letter
          Result.push_back('a' + (Byte % 26));
        } else {
          // Rest can be letter or digit
          unsigned Val = Byte % 36;
          if (Val < 26)
            Result.push_back('a' + Val);
          else
            Result.push_back('0' + (Val - 26));
        }
      }
      Round++;
    }

    return Result;
  }

  /// Hash a method selector, preserving colons and hashing each component.
  /// e.g. "IGP_foo:bar:" -> "a7k2m9x:p3n8r:" (each part hashed to same length)
  std::string hashSelector(StringRef Selector) {
    std::string Result;
    // Split on colons
    StringRef Remaining = Selector;
    bool HasColons = Remaining.contains(':');

    if (!HasColons) {
      return hashComponent(Selector);
    }

    while (!Remaining.empty()) {
      size_t ColonPos = Remaining.find(':');
      if (ColonPos == StringRef::npos) {
        // Trailing part after last colon (shouldn't happen for well-formed
        // selectors but handle it)
        Result += hashComponent(Remaining);
        break;
      }
      StringRef Component = Remaining.substr(0, ColonPos);
      if (!Component.empty()) {
        Result += hashComponent(Component);
      }
      Result += ':';
      Remaining = Remaining.substr(ColonPos + 1);
    }

    return Result;
  }

  /// Look up or compute a hashed name for the given original name.
  /// For method selectors, handles the full selector including colons.
  std::string getOrComputeHash(StringRef Original) {
    std::string Key = Original.str();
    auto It = RenameMap.find(Key);
    if (It != RenameMap.end())
      return It->second;

    std::string Hashed = hashSelector(Original);
    RenameMap[Key] = Hashed;
    return Hashed;
  }

  /// Check if a name contains the prefix (possibly as a setter).
  /// Returns true if the name should be renamed.
  bool shouldRename(StringRef Name) {
    StringRef Prefix = ObjCRenamePrefix;
    if (Name.contains(Prefix))
      return true;
    return false;
  }

  /// Process a setter method name like "setIGP_foo:" and return the renamed
  /// version "setAbcdef:" keeping setter/getter in sync.
  std::string processSetterName(StringRef SetterName) {
    StringRef Prefix = ObjCRenamePrefix;
    // Pattern: "set" + PrefixedName + ":"
    // e.g. "setIGP_testProp:"
    if (!SetterName.starts_with("set"))
      return "";

    StringRef AfterSet = SetterName.substr(3); // strip "set"
    // AfterSet should be like "IGP_testProp:" or "IGP_testProp"
    StringRef WithoutColon = AfterSet;
    bool HasTrailingColon = AfterSet.ends_with(":");
    if (HasTrailingColon)
      WithoutColon = AfterSet.substr(0, AfterSet.size() - 1);

    if (!WithoutColon.starts_with(Prefix))
      return "";

    // Get the hash for the property name (without "set" prefix)
    std::string PropHashed = getOrComputeHash(WithoutColon);

    // Capitalize first char for setter
    std::string SetterHashed = "set";
    if (!PropHashed.empty()) {
      SetterHashed += (char)toupper((unsigned char)PropHashed[0]);
      SetterHashed += PropHashed.substr(1);
    }
    if (HasTrailingColon)
      SetterHashed += ':';

    return SetterHashed;
  }

  /// Rename all occurrences of prefix-matching names in a property attribute
  /// string. e.g. T@"IGP_ClassName",&,N,VIGP_PropName
  std::string processPropertyAttributes(StringRef Attrs) {
    std::string Result = Attrs.str();
    StringRef Prefix = ObjCRenamePrefix;

    // Replace class name after T@"
    size_t TPos = Result.find("T@\"");
    if (TPos != std::string::npos) {
      size_t Start = TPos + 3;
      size_t End = Result.find('"', Start);
      if (End != std::string::npos) {
        std::string ClassName = Result.substr(Start, End - Start);
        if (StringRef(ClassName).starts_with(Prefix)) {
          std::string Hashed = getOrComputeHash(ClassName);
          Result.replace(Start, End - Start, Hashed);
        }
      }
    }

    // Replace property/ivar name after last ,V
    size_t VPos = Result.rfind(",V");
    if (VPos != std::string::npos) {
      size_t Start = VPos + 2;
      std::string IvarName = Result.substr(Start);
      if (StringRef(IvarName).starts_with(Prefix)) {
        std::string Hashed = getOrComputeHash(IvarName);
        Result.replace(Start, IvarName.size(), Hashed);
      } else if (StringRef(IvarName).starts_with("_") &&
                 StringRef(IvarName).substr(1).starts_with(Prefix)) {
        // Ivar name with _ prefix: _IGP_foo -> _hashedname
        std::string Hashed = "_" + getOrComputeHash(StringRef(IvarName).substr(1));
        Result.replace(Start, IvarName.size(), Hashed);
      }
    }

    return Result;
  }

  /// Replace the string content of a global variable with a new string of the
  /// same length.
  void replaceGlobalString(GlobalVariable *GV, StringRef NewStr) {
    LLVMContext &Ctx = GV->getContext();
    // Create new constant with null terminator
    std::string WithNull(NewStr.str());
    WithNull.push_back('\0');
    Constant *NewInit =
        ConstantDataArray::get(Ctx, ArrayRef<uint8_t>((const uint8_t *)WithNull.data(),
                                                       WithNull.size()));
    GV->setInitializer(NewInit);
  }

  /// Rename a single string based on what section it's in and its content.
  /// Returns true if the string was renamed.
  bool processGlobalString(GlobalVariable *GV, StringRef Section) {
    if (!GV->hasInitializer())
      return false;

    auto *CDS = dyn_cast<ConstantDataSequential>(GV->getInitializer());
    if (!CDS || !CDS->isCString())
      return false;

    StringRef Str = CDS->getAsCString();
    StringRef Prefix = ObjCRenamePrefix;

    if (Section.contains("__objc_classname") ||
        Section.contains("__objc_propname")) {
      // Simple names: hash the whole thing if it starts with or contains the
      // prefix
      if (Str.starts_with(Prefix)) {
        std::string Hashed = getOrComputeHash(Str);
        replaceGlobalString(GV, Hashed);
        return true;
      }
    } else if (Section.contains("__objc_methname")) {
      // Method names / selectors
      if (Str.starts_with(Prefix)) {
        // Direct prefix match: hash the selector
        std::string Hashed = getOrComputeHash(Str);
        replaceGlobalString(GV, Hashed);
        return true;
      }
      // Check for ivar name pattern: _IGP_foo (underscore + prefix)
      if (Str.starts_with("_") && Str.substr(1).starts_with(Prefix)) {
        StringRef Inner = Str.substr(1); // strip leading _
        std::string Hashed = "_" + getOrComputeHash(Inner);
        if (Hashed.size() == Str.size()) {
          replaceGlobalString(GV, Hashed);
          return true;
        }
      }
      // Check for setter pattern
      if (Str.starts_with("set")) {
        std::string SetterResult = processSetterName(Str);
        if (!SetterResult.empty()) {
          // Verify length matches
          if (SetterResult.size() == Str.size()) {
            replaceGlobalString(GV, SetterResult);
            return true;
          } else {
            errs() << "ObjCRename: WARNING: setter length mismatch for " << Str
                   << " (" << Str.size() << " vs " << SetterResult.size()
                   << "), skipping\n";
          }
        }
      }
      // Check if it's a property attribute string containing the prefix
      if (Str.contains(Prefix) && (Str.contains("T@") || Str.contains(",V"))) {
        std::string Processed = processPropertyAttributes(Str);
        if (Processed != Str && Processed.size() == Str.size()) {
          replaceGlobalString(GV, Processed);
          return true;
        }
      }
    } else if (Section.contains("swift5")) {
      // Swift metadata strings
      if (Str.contains(Prefix)) {
        // For swift reflection strings, replace prefix-matching substrings
        std::string Result = Str.str();
        std::string PrefixStr = Prefix.str();
        size_t Pos = 0;
        bool Changed = false;
        while ((Pos = Result.find(PrefixStr, Pos)) != std::string::npos) {
          // Find the end of this identifier
          size_t End = Pos;
          while (End < Result.size() && Result[End] != '\0' &&
                 Result[End] != ' ' && Result[End] != ',' &&
                 Result[End] != ';' && Result[End] != '"' &&
                 Result[End] != '<' && Result[End] != '>' &&
                 Result[End] != '(' && Result[End] != ')' &&
                 Result[End] != '.')
            End++;
          std::string Name = Result.substr(Pos, End - Pos);
          std::string Hashed = getOrComputeHash(Name);
          if (Hashed.size() == Name.size()) {
            Result.replace(Pos, Name.size(), Hashed);
            Changed = true;
          }
          Pos = End;
        }
        if (Changed && Result.size() == Str.size()) {
          replaceGlobalString(GV, Result);
          return true;
        }
      }
    }

    return false;
  }

  void writeMappingFile(Module &M) {
    if (RenameMap.empty())
      return;

    SmallString<256> HomeDir;
    if (!sys::path::home_directory(HomeDir))
      return;

    // Build path: ~/modulename_objc_rename.map
    std::string ModName = M.getSourceFileName();
    // Strip path, keep just filename
    size_t LastSlash = ModName.rfind('/');
    if (LastSlash != std::string::npos)
      ModName = ModName.substr(LastSlash + 1);
    // Strip extension
    size_t DotPos = ModName.rfind('.');
    if (DotPos != std::string::npos)
      ModName = ModName.substr(0, DotPos);

    SmallString<256> MapPath = HomeDir;
    sys::path::append(MapPath, ModName + "_objc_rename.map");

    std::error_code EC;
    raw_fd_ostream OS(MapPath, EC, sys::fs::OF_Append);
    if (EC) {
      errs() << "ObjCRename: WARNING: Could not open mapping file " << MapPath
             << ": " << EC.message() << "\n";
      return;
    }

    for (const auto &Entry : RenameMap) {
      OS << Entry.first << " -> " << Entry.second << "\n";
    }
  }

  bool runOnModule(Module &M) override {
    if (!flag)
      return false;

    errs() << "Running ObjCRename on " << M.getSourceFileName() << "\n";
    errs() << "ObjCRename prefix: " << ObjCRenamePrefix << "\n";

    StringRef Prefix = ObjCRenamePrefix;
    bool Changed = false;

    // Collect globals to process (don't modify while iterating)
    SmallVector<std::pair<GlobalVariable *, std::string>, 32> ToProcess;

    for (GlobalVariable &GV : M.globals()) {
      if (!GV.hasInitializer())
        continue;
      if (!GV.hasSection())
        continue;

      StringRef Section = GV.getSection();
      if (Section.contains("__objc_classname") ||
          Section.contains("__objc_methname") ||
          Section.contains("__objc_propname") ||
          Section.contains("swift5")) {
        // Only process string globals
        if (isa<ConstantDataSequential>(GV.getInitializer())) {
          auto *CDS = cast<ConstantDataSequential>(GV.getInitializer());
          if (CDS->isCString()) {
            StringRef Str = CDS->getAsCString();
            if (shouldRename(Str) ||
                (Str.starts_with("set") && Str.size() > 3 &&
                 shouldRename(Str.substr(3))) ||
                (Str.starts_with("_") && Str.size() > 1 &&
                 shouldRename(Str.substr(1)))) {
              ToProcess.push_back({&GV, Section.str()});
            }
          }
        }
      }
    }

    // First pass: process class names and property names to build the map
    for (auto &[GV, Section] : ToProcess) {
      StringRef Sec(Section);
      if (Sec.contains("__objc_classname") || Sec.contains("__objc_propname")) {
        if (processGlobalString(GV, Sec))
          Changed = true;
      }
    }

    // Second pass: process method names (setters need the map from first pass)
    for (auto &[GV, Section] : ToProcess) {
      StringRef Sec(Section);
      if (Sec.contains("__objc_methname")) {
        if (processGlobalString(GV, Sec))
          Changed = true;
      }
    }

    // Third pass: Swift metadata
    for (auto &[GV, Section] : ToProcess) {
      StringRef Sec(Section);
      if (Sec.contains("swift5")) {
        if (processGlobalString(GV, Sec))
          Changed = true;
      }
    }

    // Fourth pass: unsectioned string constants (Swift ivar names, type
    // descriptor names, and _TtC mangled class names)
    if (!RenameMap.empty()) {
      for (GlobalVariable &GV : M.globals()) {
        if (!GV.hasInitializer() || GV.hasSection())
          continue;
        auto *CDS = dyn_cast<ConstantDataSequential>(GV.getInitializer());
        if (!CDS || !CDS->isCString())
          continue;
        StringRef Str = CDS->getAsCString();
        if (!Str.contains(Prefix))
          continue;

        if (Str.starts_with(Prefix)) {
          // Exact match: e.g. "IGP_SwiftClass", "IGP_name"
          // Check if we already have a mapping (from sectioned globals)
          std::string Hashed = getOrComputeHash(Str);
          if (Hashed.size() == Str.size()) {
            replaceGlobalString(&GV, Hashed);
            Changed = true;
          }
        } else {
          // Substring replacement: e.g. "_TtC17test_swift_rename14IGP_SwiftClass"
          // Replace all occurrences of known prefixed names with their hashes
          std::string Result = Str.str();
          std::string PrefixStr = Prefix.str();
          size_t Pos = 0;
          bool StrChanged = false;
          while ((Pos = Result.find(PrefixStr, Pos)) != std::string::npos) {
            // Find the end of this identifier
            size_t End = Pos;
            while (End < Result.size() &&
                   (isalnum((unsigned char)Result[End]) || Result[End] == '_'))
              End++;
            std::string Name = Result.substr(Pos, End - Pos);
            std::string Hashed = getOrComputeHash(Name);
            if (Hashed.size() == Name.size()) {
              Result.replace(Pos, Name.size(), Hashed);
              StrChanged = true;
              Pos = End;
            } else {
              Pos = End;
            }
          }
          if (StrChanged && Result.size() == Str.size()) {
            replaceGlobalString(&GV, Result);
            Changed = true;
          }
        }
      }
    }

    // Fifth pass: rename class names in Swift mangled symbol names
    // Class names appear literally as <length><name> in manglings, e.g.
    // $s10TestModule14IGP_SwiftClassCMn — since our hash is length-preserving,
    // the length prefix stays valid. LLVM setName() updates all references.
    if (!RenameMap.empty()) {
      // Collect name replacements to apply (can't mutate while iterating)
      SmallVector<std::pair<GlobalValue *, std::string>, 64> SymbolRenames;

      auto collectRenames = [&](GlobalValue &GV) {
        if (!GV.hasName())
          return;
        StringRef Name = GV.getName();
        if (!Name.contains(Prefix))
          return;

        std::string NewName = Name.str();
        std::string PrefixStr = Prefix.str();
        size_t Pos = 0;
        bool NameChanged = false;
        while ((Pos = NewName.find(PrefixStr, Pos)) != std::string::npos) {
          // Find end of identifier
          size_t End = Pos;
          while (End < NewName.size() &&
                 (isalnum((unsigned char)NewName[End]) || NewName[End] == '_'))
            End++;
          std::string Ident = NewName.substr(Pos, End - Pos);
          std::string Hashed = getOrComputeHash(Ident);
          if (Hashed.size() == Ident.size()) {
            NewName.replace(Pos, Ident.size(), Hashed);
            NameChanged = true;
          }
          Pos = End;
        }
        if (NameChanged)
          SymbolRenames.push_back({&GV, NewName});
      };

      for (GlobalVariable &GV : M.globals())
        collectRenames(GV);
      for (Function &F : M)
        collectRenames(F);
      for (GlobalAlias &GA : M.aliases())
        collectRenames(GA);

      for (auto &[GV, NewName] : SymbolRenames) {
        GV->setName(NewName);
        Changed = true;
      }

      if (!SymbolRenames.empty())
        errs() << "ObjCRename: Renamed " << SymbolRenames.size()
               << " mangled symbols\n";
    }

    // Write mapping file
    if (Changed) {
      writeMappingFile(M);
      errs() << "ObjCRename: Renamed " << RenameMap.size()
             << " unique identifiers\n";
    }

    return Changed;
  }
};

ModulePass *createObjCRenamePass(bool flag) { return new ObjCRename(flag); }

} // namespace llvm

char ObjCRename::ID = 0;
INITIALIZE_PASS(ObjCRename, "objcren", "Enable ObjC Symbol Renaming", false,
                false)

//===--- Headers.cpp - Include headers ---------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Headers.h"
#include "Preamble.h"
#include "SourceCode.h"
#include "support/Logger.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/DirectoryLookup.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Tooling/Inclusions/HeaderAnalysis.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Path.h"
#include <cstring>
#include <optional>
#include <string>

namespace clang {
namespace clangd {

class IncludeStructure::RecordHeaders : public PPCallbacks {
public:
  RecordHeaders(const CompilerInstance &CI, IncludeStructure *Out)
      : SM(CI.getSourceManager()), Out(Out) {}

  // Record existing #includes - both written and resolved paths. Only #includes
  // in the main file are collected.
  void InclusionDirective(SourceLocation HashLoc, const Token &IncludeTok,
                          llvm::StringRef FileName, bool IsAngled,
                          CharSourceRange /*FilenameRange*/,
                          OptionalFileEntryRef File,
                          llvm::StringRef /*SearchPath*/,
                          llvm::StringRef /*RelativePath*/,
                          const clang::Module * /*SuggestedModule*/,
                          bool /*ModuleImported*/,
                          SrcMgr::CharacteristicKind FileKind) override {
    auto MainFID = SM.getMainFileID();
    // If an include is part of the preamble patch, translate #line directives.
    if (InBuiltinFile)
      HashLoc = translatePreamblePatchLocation(HashLoc, SM);

    // Record main-file inclusions (including those mapped from the preamble
    // patch).
    if (isInsideMainFile(HashLoc, SM)) {
      Out->MainFileIncludes.emplace_back();
      auto &Inc = Out->MainFileIncludes.back();
      Inc.Written =
          (IsAngled ? "<" + FileName + ">" : "\"" + FileName + "\"").str();
      Inc.Resolved = std::string(
          File ? getCanonicalPath(*File, SM.getFileManager()).value_or("")
               : "");
      Inc.HashOffset = SM.getFileOffset(HashLoc);
      Inc.HashLine =
          SM.getLineNumber(SM.getFileID(HashLoc), Inc.HashOffset) - 1;
      Inc.FileKind = FileKind;
      Inc.Directive = IncludeTok.getIdentifierInfo()->getPPKeywordID();
      if (File) {
        IncludeStructure::HeaderID HID = Out->getOrCreateID(*File);
        Inc.HeaderID = static_cast<unsigned>(HID);
        if (IsAngled)
          if (auto StdlibHeader = tooling::stdlib::Header::named(Inc.Written)) {
            auto &IDs = Out->StdlibHeaders[*StdlibHeader];
            // Few physical files for one stdlib header name, linear scan is ok.
            if (!llvm::is_contained(IDs, HID))
              IDs.push_back(HID);
          }
      }
      Out->MainFileIncludesBySpelling[Inc.Written].push_back(
          Out->MainFileIncludes.size() - 1);
    }

    // Record include graph (not just for main-file includes)
    if (File) {
      auto IncludingFileEntry = SM.getFileEntryRefForID(SM.getFileID(HashLoc));
      if (!IncludingFileEntry) {
        assert(SM.getBufferName(HashLoc).starts_with("<") &&
               "Expected #include location to be a file or <built-in>");
        // Treat as if included from the main file.
        IncludingFileEntry = SM.getFileEntryRefForID(MainFID);
      }
      auto IncludingID = Out->getOrCreateID(*IncludingFileEntry),
           IncludedID = Out->getOrCreateID(*File);
      Out->IncludeChildren[IncludingID].push_back(IncludedID);
    }
  }

  void FileChanged(SourceLocation Loc, FileChangeReason Reason,
                   SrcMgr::CharacteristicKind FileType,
                   FileID PrevFID) override {
    switch (Reason) {
    case PPCallbacks::EnterFile:
      ++Level;
      if (BuiltinFile.isInvalid() && SM.isWrittenInBuiltinFile(Loc)) {
        BuiltinFile = SM.getFileID(Loc);
        InBuiltinFile = true;
      }
      break;
    case PPCallbacks::ExitFile: {
      --Level;
      if (PrevFID == BuiltinFile)
        InBuiltinFile = false;
      break;
    }
    case PPCallbacks::RenameFile:
    case PPCallbacks::SystemHeaderPragma:
      break;
    }
  }

private:
  // Keeps track of include depth for the current file. It's 1 for main file.
  int Level = 0;
  bool inMainFile() const { return Level == 1; }

  const SourceManager &SM;
  // Set after entering the <built-in> file.
  FileID BuiltinFile;
  // Indicates whether <built-in> file is part of include stack.
  bool InBuiltinFile = false;

  IncludeStructure *Out;
};

bool isLiteralInclude(llvm::StringRef Include) {
  return Include.starts_with("<") || Include.starts_with("\"");
}

bool HeaderFile::valid() const {
  return (Verbatim && isLiteralInclude(File)) ||
         (!Verbatim && llvm::sys::path::is_absolute(File));
}

llvm::Expected<HeaderFile> toHeaderFile(llvm::StringRef Header,
                                        llvm::StringRef HintPath) {
  if (isLiteralInclude(Header))
    return HeaderFile{Header.str(), /*Verbatim=*/true};
  auto U = URI::parse(Header);
  if (!U)
    return U.takeError();

  auto IncludePath = URI::includeSpelling(*U);
  if (!IncludePath)
    return IncludePath.takeError();
  if (!IncludePath->empty())
    return HeaderFile{std::move(*IncludePath), /*Verbatim=*/true};

  auto Resolved = URI::resolve(*U, HintPath);
  if (!Resolved)
    return Resolved.takeError();
  return HeaderFile{std::move(*Resolved), /*Verbatim=*/false};
}

llvm::SmallVector<SymbolInclude, 1> getRankedIncludes(const Symbol &Sym) {
  auto Includes = Sym.IncludeHeaders;
  // Sort in descending order by reference count and header length.
  llvm::sort(Includes, [](const Symbol::IncludeHeaderWithReferences &LHS,
                          const Symbol::IncludeHeaderWithReferences &RHS) {
    if (LHS.References == RHS.References)
      return LHS.IncludeHeader.size() < RHS.IncludeHeader.size();
    return LHS.References > RHS.References;
  });
  llvm::SmallVector<SymbolInclude, 1> Headers;
  for (const auto &Include : Includes)
    Headers.push_back({Include.IncludeHeader, Include.supportedDirectives()});
  return Headers;
}

void IncludeStructure::collect(const CompilerInstance &CI) {
  auto &SM = CI.getSourceManager();
  MainFileEntry = SM.getFileEntryForID(SM.getMainFileID());
  auto Collector = std::make_unique<RecordHeaders>(CI, this);
  CI.getPreprocessor().addPPCallbacks(std::move(Collector));

  // If we're reusing a preamble, don't repopulate SearchPathsCanonical.
  // The entries will be the same, but canonicalizing to find out is expensive!
  if (SearchPathsCanonical.empty()) {
    for (const auto &Dir :
         CI.getPreprocessor().getHeaderSearchInfo().search_dir_range()) {
      if (Dir.getLookupType() == DirectoryLookup::LT_NormalDir)
        SearchPathsCanonical.emplace_back(
            SM.getFileManager().getCanonicalName(*Dir.getDirRef()));
    }
  }
}

std::optional<IncludeStructure::HeaderID>
IncludeStructure::getID(const FileEntry *Entry) const {
  // HeaderID of the main file is always 0;
  if (Entry == MainFileEntry) {
    return static_cast<IncludeStructure::HeaderID>(0u);
  }
  auto It = UIDToIndex.find(Entry->getUniqueID());
  if (It == UIDToIndex.end())
    return std::nullopt;
  return It->second;
}

IncludeStructure::HeaderID IncludeStructure::getOrCreateID(FileEntryRef Entry) {
  // Main file's FileEntry was not known at IncludeStructure creation time.
  if (&Entry.getFileEntry() == MainFileEntry) {
    if (RealPathNames.front().empty())
      RealPathNames.front() = MainFileEntry->tryGetRealPathName().str();
    return MainFileID;
  }
  auto R = UIDToIndex.try_emplace(
      Entry.getUniqueID(),
      static_cast<IncludeStructure::HeaderID>(RealPathNames.size()));
  if (R.second)
    RealPathNames.emplace_back();
  IncludeStructure::HeaderID Result = R.first->getSecond();
  std::string &RealPathName = RealPathNames[static_cast<unsigned>(Result)];
  if (RealPathName.empty())
    RealPathName = Entry.getFileEntry().tryGetRealPathName().str();
  return Result;
}

llvm::DenseMap<IncludeStructure::HeaderID, unsigned>
IncludeStructure::includeDepth(HeaderID Root) const {
  // Include depth 0 is the main file only.
  llvm::DenseMap<HeaderID, unsigned> Result;
  assert(static_cast<unsigned>(Root) < RealPathNames.size());
  Result[Root] = 0;
  std::vector<IncludeStructure::HeaderID> CurrentLevel;
  CurrentLevel.push_back(Root);
  llvm::DenseSet<IncludeStructure::HeaderID> Seen;
  Seen.insert(Root);

  // Each round of BFS traversal finds the next depth level.
  std::vector<IncludeStructure::HeaderID> PreviousLevel;
  for (unsigned Level = 1; !CurrentLevel.empty(); ++Level) {
    PreviousLevel.clear();
    PreviousLevel.swap(CurrentLevel);
    for (const auto &Parent : PreviousLevel) {
      for (const auto &Child : IncludeChildren.lookup(Parent)) {
        if (Seen.insert(Child).second) {
          CurrentLevel.push_back(Child);
          Result[Child] = Level;
        }
      }
    }
  }
  return Result;
}

llvm::SmallVector<const Inclusion *>
IncludeStructure::mainFileIncludesWithSpelling(llvm::StringRef Spelling) const {
  llvm::SmallVector<const Inclusion *> Includes;
  for (auto Idx : MainFileIncludesBySpelling.lookup(Spelling))
    Includes.push_back(&MainFileIncludes[Idx]);
  return Includes;
}

void IncludeInserter::addExisting(const Inclusion &Inc) {
  IncludedHeaders.insert(Inc.Written);
  if (!Inc.Resolved.empty())
    IncludedHeaders.insert(Inc.Resolved);
}

/// FIXME(ioeric): we might not want to insert an absolute include path if the
/// path is not shortened.
bool IncludeInserter::shouldInsertInclude(
    PathRef DeclaringHeader, const HeaderFile &InsertedHeader) const {
  assert(InsertedHeader.valid());
  if (!HeaderSearchInfo && !InsertedHeader.Verbatim)
    return false;
  if (FileName == DeclaringHeader || FileName == InsertedHeader.File)
    return false;
  auto Included = [&](llvm::StringRef Header) {
    return IncludedHeaders.contains(Header);
  };
  return !Included(DeclaringHeader) && !Included(InsertedHeader.File);
}

std::optional<std::string>
IncludeInserter::calculateIncludePath(const HeaderFile &InsertedHeader,
                                      llvm::StringRef IncludingFile) const {
  assert(InsertedHeader.valid());
  if (InsertedHeader.Verbatim)
    return InsertedHeader.File;
  bool IsAngledByDefault = false;
  std::string Suggested;
  if (HeaderSearchInfo) {
    Suggested = HeaderSearchInfo->suggestPathToFileForDiagnostics(
        InsertedHeader.File, BuildDir, IncludingFile, &IsAngledByDefault);
  } else {
    // Calculate include relative to including file only.
    StringRef IncludingDir = llvm::sys::path::parent_path(IncludingFile);
    SmallString<256> RelFile(InsertedHeader.File);
    // Replacing with "" leaves "/RelFile" if IncludingDir doesn't end in "/".
    llvm::sys::path::replace_path_prefix(RelFile, IncludingDir, "./");
    Suggested = llvm::sys::path::convert_to_slash(
        llvm::sys::path::remove_leading_dotslash(RelFile));
  }
  // FIXME: should we allow (some limited number of) "../header.h"?
  if (llvm::sys::path::is_absolute(Suggested))
    return std::nullopt;
  bool IsAngled = false;
  for (auto &Filter : AngledHeaders) {
    if (Filter(Suggested)) {
      IsAngled = true;
      break;
    }
  }
  bool IsQuoted = false;
  for (auto &Filter : QuotedHeaders) {
    if (Filter(Suggested)) {
      IsQuoted = true;
      break;
    }
  }
  // No filters apply, or both filters apply (a bug), use system default.
  if (IsAngled == IsQuoted) {
    // Probably a bug in the config regex.
    if (IsAngled && IsQuoted) {
      elog("Header '{0}' matches both quoted and angled regexes, default will "
           "be used.",
           Suggested);
    }
    IsAngled = IsAngledByDefault;
  }
  if (IsAngled)
    Suggested = "<" + Suggested + ">";
  else // if (IsQuoted)
    Suggested = "\"" + Suggested + "\"";
  return Suggested;
}

std::optional<TextEdit>
IncludeInserter::insert(llvm::StringRef VerbatimHeader,
                        tooling::IncludeDirective Directive) const {
  std::optional<TextEdit> Edit;
  if (auto Insertion =
          Inserter.insert(VerbatimHeader.trim("\"<>"),
                          VerbatimHeader.starts_with("<"), Directive))
    Edit = replacementToEdit(Code, *Insertion);
  return Edit;
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const Inclusion &Inc) {
  return OS << Inc.Written << " = "
            << (!Inc.Resolved.empty() ? Inc.Resolved : "[unresolved]")
            << " at line" << Inc.HashLine;
}

bool operator==(const Inclusion &LHS, const Inclusion &RHS) {
  return std::tie(LHS.Directive, LHS.FileKind, LHS.HashOffset, LHS.HashLine,
                  LHS.Resolved, LHS.Written) ==
         std::tie(RHS.Directive, RHS.FileKind, RHS.HashOffset, RHS.HashLine,
                  RHS.Resolved, RHS.Written);
}

} // namespace clangd
} // namespace clang

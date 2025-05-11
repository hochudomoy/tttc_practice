#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/AST.h"
#include "clang/Frontend/CompilerInstance.h"
#include "llvm/Support/raw_ostream.h"
using namespace clang;

namespace {

class UserTypeInfoConsumer : public ASTConsumer {
  CompilerInstance &Instance;

public:
  UserTypeInfoConsumer(CompilerInstance &Instance) : Instance(Instance) {}

  void HandleTranslationUnit(ASTContext &Context) override {
    TranslationUnitDecl *TUDecl = Context.getTranslationUnitDecl();
    for (auto *D : TUDecl->decls()) {
      Visit(D);
    }
  }

  void Visit(Decl *D) {
    if (auto *recordDecl = dyn_cast<CXXRecordDecl>(D)) {
      if (recordDecl->isCompleteDefinition() &&
          !recordDecl->isImplicit()) {
        PrintInfo(recordDecl);
      }
    }
  }


 private:
     void PrintInfo(const CXXRecordDecl* recordDecl) {
       llvm::outs() << recordDecl->getNameAsString();
       for (const CXXBaseSpecifier& base : recordDecl->bases()) {
         const CXXRecordDecl *baseDecl = base.getType()->getAsCXXRecordDecl();
         if (baseDecl) {
           llvm::outs() << "->" << baseDecl->getNameAsString();
         }
       }
       llvm::outs() << "\n";
       PrintFields(recordDecl);
       PrintMethods(recordDecl);
  }

     void PrintFields(const CXXRecordDecl *recordDecl) {
       llvm::outs() <<"|Fields\n";
       std::string access;
       for (const auto *field : recordDecl->fields()) {
         std::string access;
         if (field->getAccess() == AS_public) {
           access = "public";
         } else if (field->getAccess() == AS_private) {
           access = "private";
         } else {
           access = "protected";
         }
         llvm::outs() << "| |_" << field->getNameAsString() << "("
                      << field->getType().getAsString() << "|" << access
                      << ")\n";
       }
     }
     void PrintMethods(const CXXRecordDecl *recordDecl) {
       llvm::outs() << "|Methods\n";
       for (const auto *method : recordDecl->methods()) {
         if (method->isImplicit())continue;
         std::string access;
         if (method->getAccess() == AS_public) {
           access = "public";
         } else if (method->getAccess() == AS_private) {
           access = "private";
         } else {
           access = "protected";
         }

         std::string identificators;
         if (method->isVirtual()) {
           identificators += "| virtual";
         }
         if (method->isPureVirtual()) {
           identificators += "| pure";
         }
         if (method->size_overridden_methods() > 0) {
           identificators += "| override";
         }
         llvm::outs() << "| |_" << method->getNameAsString() << "("
                      << method->getReturnType().getAsString()<< "() |" << access
                      << identificators << ")\n";

       }
     }
};


class UserTypeInfoAction : public PluginASTAction {
protected:
  bool ParseArgs(const CompilerInstance &CI,
                 const std::vector<std::string> &args) override {
    return true;
  }
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                 llvm::StringRef) override {
    return std::make_unique<UserTypeInfoConsumer>(CI);
  };
};
} 
static FrontendPluginRegistry::Add<UserTypeInfoAction> X("user-type-info",
                                               "Prints information about user data types");
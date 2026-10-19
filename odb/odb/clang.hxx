// file      : odb/clang.hxx
// license   : GNU GPL v3; see accompanying LICENSE file

#ifndef ODB_CLANG_HXX
#define ODB_CLANG_HXX

// Clang-based implementation of the compiler interface that the ODB
// compiler used to get from the GCC plugin headers. To keep the bulk of
// the original code intact we provide a subset of the GCC tree API (with
// the same names and semantics where it makes sense) on top of the Clang
// AST. See clang-fwd.hxx for the basic types.
//
// Mapping overview:
//
// GCC                        Clang
// ---                        -----
// NAMESPACE_DECL             NamespaceDecl (canonical), TranslationUnitDecl
// TYPE_DECL                  TypeDecl (TagDecl, TypedefNameDecl) or a type
//                            (implicit TYPE_DECL, e.g., for fundamentals)
// RECORD_TYPE/UNION_TYPE...  QualType (canonical for TYPE_MAIN_VARIANT)
// typedef'ed type variant    TypedefType sugar
// INTEGER_CST/REAL_CST       tree_int_cst/tree_real_cst (pragma numbers)
//

#include <odb/clang-fwd.hxx>

#include <string>
#include <vector>

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/Type.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Basic/TokenKinds.h>
#include <clang/Sema/Sema.h>

// Numeric constant nodes (produced by the pragma lexer).
//
enum cst_type
{
  cst_int,
  cst_unsigned,
  cst_long,
  cst_unsigned_long,
  cst_long_long,
  cst_unsigned_long_long,
  cst_float,
  cst_double,
  cst_long_double
};

struct tree_int_cst
{
  cst_type type;
  unsigned long long value;
};

struct tree_real_cst
{
  cst_type type;
  double value;
  std::string literal; // Original spelling.
};

tree
make_int_cst (cst_type, unsigned long long);

tree
make_real_cst (cst_type, double, std::string const& literal);

// Compiler state (set by the frontend).
//
namespace odbc
{
  extern clang::ASTContext* ast_context;
  extern clang::Sema* sema_;
  extern clang::SourceManager* source_manager;
  extern clang::FileID main_file_id;

  inline clang::ASTContext&
  ast () {return *ast_context;}

  inline clang::SourceManager&
  sm () {return *source_manager;}

  // Conversions between tree nodes and Clang entities.
  //
  tree
  make (clang::Decl const*);

  tree
  make (clang::QualType);

  tree
  make_type_decl (clang::QualType);

  // Return the scope tree for the declaration context: global_namespace
  // for the translation unit, NAMESPACE_DECL for namespaces, and the
  // class type for classes. Transparent contexts (extern "C", etc) are
  // skipped.
  //
  tree
  make_scope (clang::DeclContext const*);

  clang::Decl const*
  decl (tree); // NULL if not a decl.

  clang::QualType
  type (tree); // Null if not a type.

  clang::DeclContext*
  decl_context (tree scope); // For NAMESPACE_DECL, class type, etc.

  clang::CXXRecordDecl*
  record_decl (tree type); // NULL if not a C++ class/struct/union type.

  location_t
  make_location (clang::SourceLocation);

  clang::SourceLocation
  source_location (location_t);

  // Print a type name similar to GCC's type_as_string(TFF_PLAIN_IDENTIFIER)
  // (inline namespaces and default template arguments are omitted).
  //
  std::string
  type_as_string (clang::QualType, bool unqualified = false);

  // Return true if this namespace is inline.
  //
  bool
  inline_namespace (tree ns);

  // Return true if this type is anonymous (unnamed class/enum without a
  // typedef name for linkage purposes).
  //
  bool
  anonymous_type (tree type);

  // Instantiate the class template with a single type argument. Return
  // null if the instantiation cannot be completed and throw failed if
  // the template-id is invalid.
  //
  struct failed {};

  tree
  instantiate_template (tree class_template, tree arg);

  // Lookup a static data member in a class and get its integral value.
  // Return false if not found or not an integral constant.
  //
  bool
  static_member_value (tree type, char const* name, unsigned long long&);

  // Member functions of a class with the given name.
  //
  struct member_function
  {
    clang::CXXMethodDecl const* decl;
    bool static_;
    bool const_;
    bool deleted;
    clang::AccessSpecifier access;
    std::vector<clang::QualType> params;
    clang::QualType result;
  };

  // Return false if there is no such name or it doesn't refer to member
  // functions.
  //
  bool
  lookup_member_functions (tree type,
                           std::string const& name,
                           std::vector<member_function>&);

  // Return true if class f is a friend of class c.
  //
  bool
  is_friend (tree c, tree f);

  // Return the (main variant) pointer type to the type.
  //
  tree
  pointer_to (tree type);

  // If the type is (or is an alias for) a class template specialization,
  // return the most general class template declaration. Otherwise return
  // null.
  //
  tree
  class_template_of (tree type);

  // Fully-qualified declaration name without the leading '::' and inline
  // namespaces (similar to GCC's decl_as_string(TFF_PLAIN_IDENTIFIER)).
  //
  std::string
  qualified_name (tree decl);

  // Check if the type has operator< (as used by session support).
  //
  bool
  has_lt_operator (tree type);

  // Map Clang token kind to the libcpp token type (keywords are mapped
  // to CPP_KEYWORD). Return N_TTYPES if there is no mapping.
  //
  cpp_ttype
  token_type (clang::tok::TokenKind);

  // Return true if the identifier is a keyword in the current dialect.
  //
  bool
  keyword_p (llvm::StringRef);

  // Include tracking: for each file included into the translation unit
  // return the inclusion site.
  //
  struct include_site
  {
    std::string included; // Included file path.
    std::string file;     // File (presumed name) containing the #include.
    unsigned int line;    // Line of the #include directive.
    bool main;            // #include is in the main file.
    unsigned int id;      // Unique id (FileID) for ordering.
  };

  std::vector<include_site>
  include_sites ();

  // Return the inclusion site of the file containing the location or
  // false if it is in the main file (or invalid).
  //
  bool
  included_from (location_t, include_site&);
}

#endif // ODB_CLANG_HXX

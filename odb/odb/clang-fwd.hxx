// file      : odb/clang-fwd.hxx
// license   : GNU GPL v3; see accompanying LICENSE file

#ifndef ODB_CLANG_FWD_HXX
#define ODB_CLANG_FWD_HXX

// Lightweight (Clang headers-free) definitions of the compiler-related
// types that are used throughout the ODB compiler: the tree node handle,
// the source location, and the C++ token types. These used to be provided
// by the GCC plugin headers and the rest of the code is written in terms
// of them. See clang.hxx for the Clang-based implementation details.
//

#include <cstddef> // std::nullptr_t
#include <string>

// Tree node handle. Refers to a Clang declaration, type, or a numeric
// constant (as produced by the pragma lexer).
//
class tree
{
public:
  enum kind_type
  {
    null_kind,
    error_kind,     // error_mark_node.
    decl_kind,      // clang::Decl const*.
    type_kind,      // clang::QualType (opaque pointer).
    type_decl_kind, // Implicit TYPE_DECL for a type (e.g., fundamental).
    int_cst_kind,   // tree_int_cst const*.
    real_cst_kind   // tree_real_cst const*.
  };

  tree (): kind_ (null_kind), ptr_ (0) {}
  tree (std::nullptr_t): kind_ (null_kind), ptr_ (0) {}
  tree (kind_type k, void const* p): kind_ (k), ptr_ (p) {}

  kind_type
  kind () const {return kind_;}

  void const*
  ptr () const {return ptr_;}

  explicit
  operator bool () const {return kind_ != null_kind;}

  friend bool
  operator== (tree x, tree y)
  {
    return x.kind_ == y.kind_ && x.ptr_ == y.ptr_;
  }

  friend bool
  operator!= (tree x, tree y)
  {
    return !(x == y);
  }

  friend bool
  operator< (tree x, tree y)
  {
    return x.kind_ < y.kind_ || (x.kind_ == y.kind_ && x.ptr_ < y.ptr_);
  }

private:
  kind_type kind_;
  void const* ptr_;
};

// Canonicalized keys used by semantics::unit to map tree nodes to graph
// nodes and name hints (see clang.cxx).
//
tree
tree_node_key (tree);

tree
tree_hint_key (tree);

// Source location. Wraps the raw encoding of a (file) clang::SourceLocation.
// Locations are ordered according to their position in the translation unit
// with the invalid location (0) coming first and ~location_t (0) last.
//
class location_t
{
public:
  typedef unsigned int raw_type;

  location_t (): raw_ (0) {}
  location_t (raw_type r): raw_ (r) {} // Note: implicit for location_t (0).

  raw_type
  raw () const {return raw_;}

  location_t
  operator~ () const {return location_t (~raw_);}

  friend bool
  operator== (location_t x, location_t y) {return x.raw_ == y.raw_;}

  friend bool
  operator!= (location_t x, location_t y) {return x.raw_ != y.raw_;}

  friend bool
  operator< (location_t x, location_t y) {return less (x.raw_, y.raw_);}

  friend bool
  operator> (location_t x, location_t y) {return less (y.raw_, x.raw_);}

  friend bool
  operator<= (location_t x, location_t y) {return !less (y.raw_, x.raw_);}

  friend bool
  operator>= (location_t x, location_t y) {return !less (x.raw_, y.raw_);}

private:
  static bool
  less (raw_type, raw_type);

private:
  raw_type raw_;
};

const location_t UNKNOWN_LOCATION (0);
const location_t BUILTINS_LOCATION (0);

// Presumed (#line-aware) location components.
//
char const*
presumed_file (location_t);

unsigned int
presumed_line (location_t);

unsigned int
presumed_column (location_t);

#define LOCATION_FILE(L) (presumed_file (L))
#define LOCATION_LINE(L) (presumed_line (L))
#define LOCATION_COLUMN(L) (presumed_column (L))

// C++ token types. These are the same as the libcpp ones (see GCC's
// cpplib.h) since the code (including the generated one) relies on
// their names and ordering.
//
#define TTYPE_TABLE                             \
  OP(EQ,                "=")                    \
  OP(NOT,               "!")                    \
  OP(GREATER,           ">")                    \
  OP(LESS,              "<")                    \
  OP(PLUS,              "+")                    \
  OP(MINUS,             "-")                    \
  OP(MULT,              "*")                    \
  OP(DIV,               "/")                    \
  OP(MOD,               "%")                    \
  OP(AND,               "&")                    \
  OP(OR,                "|")                    \
  OP(XOR,               "^")                    \
  OP(RSHIFT,            ">>")                   \
  OP(LSHIFT,            "<<")                   \
                                                \
  OP(COMPL,             "~")                    \
  OP(AND_AND,           "&&")                   \
  OP(OR_OR,             "||")                   \
  OP(QUERY,             "?")                    \
  OP(COLON,             ":")                    \
  OP(COMMA,             ",")                    \
  OP(OPEN_PAREN,        "(")                    \
  OP(CLOSE_PAREN,       ")")                    \
  TK(EOF,               NONE)                   \
  OP(EQ_EQ,             "==")                   \
  OP(NOT_EQ,            "!=")                   \
  OP(GREATER_EQ,        ">=")                   \
  OP(LESS_EQ,           "<=")                   \
  OP(SPACESHIP,         "<=>")                  \
                                                \
  OP(PLUS_EQ,           "+=")                   \
  OP(MINUS_EQ,          "-=")                   \
                                                \
  OP(MULT_EQ,           "*=")                   \
  OP(DIV_EQ,            "/=")                   \
  OP(MOD_EQ,            "%=")                   \
  OP(AND_EQ,            "&=")                   \
  OP(OR_EQ,             "|=")                   \
  OP(XOR_EQ,            "^=")                   \
  OP(RSHIFT_EQ,         ">>=")                  \
  OP(LSHIFT_EQ,         "<<=")                  \
                                                \
  OP(HASH,              "#")                    \
  OP(PASTE,             "##")                   \
  OP(OPEN_SQUARE,       "[")                    \
  OP(CLOSE_SQUARE,      "]")                    \
  OP(OPEN_BRACE,        "{")                    \
  OP(CLOSE_BRACE,       "}")                    \
                                                \
  OP(SEMICOLON,         ";")                    \
  OP(ELLIPSIS,          "...")                  \
  OP(PLUS_PLUS,         "++")                   \
  OP(MINUS_MINUS,       "--")                   \
  OP(DEREF,             "->")                   \
  OP(DOT,               ".")                    \
  OP(SCOPE,             "::")                   \
  OP(DEREF_STAR,        "->*")                  \
  OP(DOT_STAR,          ".*")                   \
  OP(ATSIGN,            "@")                    \
                                                \
  TK(NAME,              IDENT)                  \
  TK(AT_NAME,           IDENT)                  \
  TK(NUMBER,            LITERAL)                \
                                                \
  TK(CHAR,              LITERAL)                \
  TK(WCHAR,             LITERAL)                \
  TK(CHAR16,            LITERAL)                \
  TK(CHAR32,            LITERAL)                \
  TK(UTF8CHAR,          LITERAL)                \
  TK(OTHER,             LITERAL)                \
                                                \
  TK(STRING,            LITERAL)                \
  TK(WSTRING,           LITERAL)                \
  TK(STRING16,          LITERAL)                \
  TK(STRING32,          LITERAL)                \
  TK(UTF8STRING,        LITERAL)                \
  TK(OBJC_STRING,       LITERAL)                \
  TK(HEADER_NAME,       LITERAL)                \
                                                \
  TK(COMMENT,           LITERAL)                \
  TK(MACRO_ARG,         NONE)                   \
  TK(PRAGMA,            NONE)                   \
  TK(PRAGMA_EOL,        NONE)                   \
  TK(PADDING,           NONE)

#define OP(e, s) CPP_ ## e,
#define TK(e, s) CPP_ ## e,
enum cpp_ttype
{
  TTYPE_TABLE
  N_TTYPES,

  // A token type for keywords, as opposed to ordinary identifiers.
  //
  CPP_KEYWORD,

  // Positions in the table.
  //
  CPP_LAST_EQ        = CPP_LSHIFT,
  CPP_FIRST_DIGRAPH  = CPP_HASH,
  CPP_LAST_PUNCTUATOR= CPP_ATSIGN,
  CPP_LAST_CPP_OP    = CPP_LESS_EQ
};
#undef OP
#undef TK

// Diagnostics counters (incremented by both ODB and Clang diagnostics).
//
extern int errorcount;
extern int warningcount;

extern tree global_namespace;
extern tree const error_mark_node;

//
// GCC tree API subset.
//

enum gcc_tree_code_type
{
  ERROR_MARK,
  IDENTIFIER_NODE,

  // Declarations.
  //
  NAMESPACE_DECL,
  TYPE_DECL,
  FIELD_DECL,
  VAR_DECL,
  CONST_DECL,
  PARM_DECL,
  FUNCTION_DECL,
  TEMPLATE_DECL,
  USING_DECL,
  BASELINK, // Member function(s).
  LAST_DECL_CODE,

  // Types.
  //
  RECORD_TYPE,
  UNION_TYPE,
  ENUMERAL_TYPE,
  VOID_TYPE,
  BOOLEAN_TYPE,
  INTEGER_TYPE,
  REAL_TYPE,
  COMPLEX_TYPE,
  VECTOR_TYPE,
  NULLPTR_TYPE,
  POINTER_TYPE,
  REFERENCE_TYPE,
  OFFSET_TYPE,
  ARRAY_TYPE,
  FUNCTION_TYPE,
  METHOD_TYPE,
  TEMPLATE_TYPE_PARM,
  TYPENAME_TYPE,
  DECLTYPE_TYPE,
  LANG_TYPE, // Other types.

  // Constants.
  //
  INTEGER_CST,
  REAL_CST
};

enum tree_code_class
{
  tcc_exceptional,
  tcc_declaration,
  tcc_type,
  tcc_constant
};

gcc_tree_code_type
TREE_CODE (tree);

tree_code_class
TREE_CODE_CLASS (gcc_tree_code_type);

char const*
gcc_tree_code_name (gcc_tree_code_type);

inline bool
TYPE_P (tree t)
{
  return TREE_CODE_CLASS (TREE_CODE (t)) == tcc_type;
}

inline bool
DECL_P (tree t)
{
  return TREE_CODE_CLASS (TREE_CODE (t)) == tcc_declaration;
}

tree
TREE_TYPE (tree);

tree
TYPE_MAIN_VARIANT (tree);

tree
TYPE_NAME (tree);

// C++ class (struct, class, or union).
//
bool
CLASS_TYPE_P (tree);

bool
COMPLETE_TYPE_P (tree);

bool
CLASSTYPE_TEMPLATE_INSTANTIATION (tree);

bool
DECL_CLASS_TEMPLATE_P (tree);

bool
TYPE_UNSIGNED (tree);

bool
TYPE_POLYMORPHIC_P (tree);

bool
CP_TYPE_CONST_P (tree);

bool
CP_TYPE_VOLATILE_P (tree);

bool
CP_TYPE_RESTRICT_P (tree);

// Scope of a declaration or type (global_namespace, NAMESPACE_DECL, or
// class type).
//
tree
CP_DECL_CONTEXT (tree);

inline tree
CP_TYPE_CONTEXT (tree t)
{
  return CP_DECL_CONTEXT (t);
}

// For typedefs, the aliased type (as written).
//
tree
DECL_ORIGINAL_TYPE (tree);

// For namespace aliases, the aliased namespace.
//
tree
ORIGINAL_NAMESPACE (tree);

std::string
DECL_NAME (tree);

location_t
real_source_location (tree decl);

inline location_t
DECL_SOURCE_LOCATION (tree decl)
{
  return real_source_location (decl);
}

#define DECL_SOURCE_FILE(D) (LOCATION_FILE (real_source_location (D)))
#define DECL_SOURCE_LINE(D) (LOCATION_LINE (real_source_location (D)))
#define DECL_SOURCE_COLUMN(D) (LOCATION_COLUMN (real_source_location (D)))

unsigned long long
integer_value (tree int_cst);

// Lookup a name in the scope (global_namespace, NAMESPACE_DECL, or class
// type) returning error_mark_node if not found. If type is true, then only
// consider declarations that name types (as well as namespaces and class
// templates). Fundamental type names (e.g., "unsigned int") are looked up
// in the global namespace. If the name refers to member functions, then
// BASELINK is returned.
//
tree
lookup_qualified_name (tree scope,
                       std::string const& name,
                       bool type,
                       bool complain);

// Make sure the class type (template instantiation) is complete. Return
// error_mark_node on failure.
//
tree
instantiate_class_template (tree type, location_t = location_t ());

#endif // ODB_CLANG_FWD_HXX

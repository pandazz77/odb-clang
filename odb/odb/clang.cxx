// file      : odb/clang.cxx
// license   : GNU GPL v3; see accompanying LICENSE file

#include <odb/clang.hxx>

#include <deque>
#include <sstream>

#include <clang/AST/Expr.h>
#include <clang/AST/PrettyPrinter.h>
#include <clang/AST/TemplateBase.h>
#include <clang/Basic/DiagnosticSema.h>
#include <clang/Sema/Lookup.h>
#include <clang/Sema/SemaDiagnostic.h>
#include <clang/Sema/Template.h>

#include <llvm/Support/raw_ostream.h>

using namespace std;
using namespace clang;

int errorcount;
int warningcount;

tree global_namespace;
tree const error_mark_node (tree::error_kind, 0);

namespace odbc
{
  ASTContext* ast_context;
  Sema* sema_;
  SourceManager* source_manager;
  FileID main_file_id;
}

using namespace odbc;

//
// Constants.
//

static deque<tree_int_cst> int_csts_;
static deque<tree_real_cst> real_csts_;

tree
make_int_cst (cst_type t, unsigned long long v)
{
  int_csts_.push_back (tree_int_cst {t, v});
  return tree (tree::int_cst_kind, &int_csts_.back ());
}

tree
make_real_cst (cst_type t, double v, string const& l)
{
  real_csts_.push_back (tree_real_cst {t, v, l});
  return tree (tree::real_cst_kind, &real_csts_.back ());
}

//
// Locations.
//

bool location_t::
less (raw_type x, raw_type y)
{
  if (x == y)
    return false;

  // Invalid location first, ~0 last.
  //
  if (x == 0 || y == raw_type (~0U))
    return true;

  if (y == 0 || x == raw_type (~0U))
    return false;

  if (source_manager == 0)
    return x < y;

  return source_manager->isBeforeInTranslationUnit (
    SourceLocation::getFromRawEncoding (x),
    SourceLocation::getFromRawEncoding (y));
}

static PresumedLoc
presumed (location_t l)
{
  if (l.raw () == 0 || l.raw () == location_t::raw_type (~0U) ||
      source_manager == 0)
    return PresumedLoc ();

  return source_manager->getPresumedLoc (source_location (l));
}

char const*
presumed_file (location_t l)
{
  PresumedLoc p (presumed (l));
  return p.isValid () ? p.getFilename () : "<built-in>";
}

unsigned int
presumed_line (location_t l)
{
  PresumedLoc p (presumed (l));
  return p.isValid () ? p.getLine () : 0;
}

unsigned int
presumed_column (location_t l)
{
  PresumedLoc p (presumed (l));
  return p.isValid () ? p.getColumn () : 0;
}

namespace odbc
{
  location_t
  make_location (SourceLocation l)
  {
    if (l.isInvalid ())
      return location_t ();

    // Like GCC's LRK_MACRO_EXPANSION_POINT.
    //
    if (l.isMacroID ())
      l = source_manager->getExpansionLoc (l);

    return location_t (l.getRawEncoding ());
  }

  SourceLocation
  source_location (location_t l)
  {
    return SourceLocation::getFromRawEncoding (l.raw ());
  }

  //
  // Tree conversions.
  //

  static Decl const*
  normalize (Decl const* d)
  {
    if (d == 0)
      return d;

    if (NamespaceDecl const* n = dyn_cast<NamespaceDecl> (d))
      return n->getCanonicalDecl ();

    if (TagDecl const* t = dyn_cast<TagDecl> (d))
    {
      // Injected class name refers to the class itself.
      //
      if (CXXRecordDecl const* r = dyn_cast<CXXRecordDecl> (t))
      {
        if (r->isInjectedClassName ())
          t = cast<CXXRecordDecl> (r->getDeclContext ());
      }

      if (TagDecl const* def = t->getDefinition ())
        return def;

      return t->getCanonicalDecl ();
    }

    if (TranslationUnitDecl const* u = dyn_cast<TranslationUnitDecl> (d))
      return u->getCanonicalDecl ();

    return d;
  }

  tree
  make (Decl const* d)
  {
    d = normalize (d);
    return d != 0 ? tree (tree::decl_kind, d) : tree ();
  }

  tree
  make (QualType t)
  {
    return !t.isNull () ? tree (tree::type_kind, t.getAsOpaquePtr ()) : tree ();
  }

  tree
  make_type_decl (QualType t)
  {
    return !t.isNull ()
      ? tree (tree::type_decl_kind, t.getAsOpaquePtr ())
      : tree ();
  }

  Decl const*
  decl (tree t)
  {
    return t.kind () == tree::decl_kind
      ? static_cast<Decl const*> (t.ptr ())
      : 0;
  }

  QualType
  type (tree t)
  {
    return t.kind () == tree::type_kind
      ? QualType::getFromOpaquePtr (t.ptr ())
      : QualType ();
  }

  static QualType
  any_type (tree t)
  {
    return t.kind () == tree::type_kind || t.kind () == tree::type_decl_kind
      ? QualType::getFromOpaquePtr (t.ptr ())
      : QualType ();
  }

  tree
  make_scope (DeclContext const* dc)
  {
    while (dc != 0 &&
           (dc->isTransparentContext () ||
            isa<LinkageSpecDecl> (dc) ||
            isa<ExportDecl> (dc)))
      dc = dc->getParent ();

    if (dc == 0)
      return tree ();

    if (isa<TranslationUnitDecl> (dc))
      return global_namespace;

    if (RecordDecl const* r = dyn_cast<RecordDecl> (dc))
      return make (QualType (ast ().getCanonicalTagType (r)));

    return make (cast<Decl> (dc));
  }

  DeclContext*
  decl_context (tree s)
  {
    if (Decl const* d = decl (s))
    {
      if (NamespaceAliasDecl const* a = dyn_cast<NamespaceAliasDecl> (d))
        d = a->getNamespace ();

      if (TagDecl const* t = dyn_cast<TagDecl> (d))
      {
        if (TagDecl const* def = t->getDefinition ())
          t = def;
      }

      if (DeclContext const* dc = dyn_cast<DeclContext> (d))
        return const_cast<DeclContext*> (dc);

      return 0;
    }

    QualType t (any_type (s));

    if (t.isNull ())
      return 0;

    if (TagDecl* td = t.getCanonicalType ()->getAsTagDecl ())
    {
      if (TagDecl* def = td->getDefinition ())
        return def;

      return td;
    }

    return 0;
  }

  CXXRecordDecl*
  record_decl (tree t)
  {
    QualType qt (any_type (t));

    if (qt.isNull ())
    {
      if (Decl const* d = decl (t))
        if (CXXRecordDecl const* r = dyn_cast<CXXRecordDecl> (d))
          return const_cast<CXXRecordDecl*> (r);

      return 0;
    }

    if (CXXRecordDecl* r = qt.getCanonicalType ()->getAsCXXRecordDecl ())
    {
      if (CXXRecordDecl* def = r->getDefinition ())
        return def;

      return r;
    }

    return 0;
  }

  //
  // Names.
  //

  static PrintingPolicy
  policy ()
  {
    PrintingPolicy p (ast ().getPrintingPolicy ());
    p.SuppressTagKeyword = true;
    p.SuppressInlineNamespace =
      llvm::to_underlying (PrintingPolicy::SuppressInlineNamespaceMode::All);
    p.SuppressDefaultTemplateArgs = true;
    p.SplitTemplateClosers = true;
    p.AnonymousTagLocations = false;
    p.SuppressUnwrittenScope = true;
    p.PrintAsCanonical = false;
    p.FullyQualifiedName = true;
    p.Bool = true;
    return p;
  }

  string
  type_as_string (QualType t, bool unqualified)
  {
    PrintingPolicy p (policy ());
    string r;
    llvm::raw_string_ostream os (r);

    if (unqualified)
    {
      QualType ct (t.getCanonicalType ());

      if (TagDecl const* td = ct->getAsTagDecl ())
      {
        if (!ct.hasLocalQualifiers ())
        {
          if (td->getIdentifier () == 0)
          {
            if (TypedefNameDecl const* tn = td->getTypedefNameForAnonDecl ())
              os << tn->getName ();
            else
              t.print (os, p); // Anonymous.
          }
          else
          {
            os << td->getName ();

            if (ClassTemplateSpecializationDecl const* s =
                dyn_cast<ClassTemplateSpecializationDecl> (td))
            {
              printTemplateArgumentList (
                os,
                s->getTemplateArgs ().asArray (),
                p,
                s->getSpecializedTemplate ()->getTemplateParameters ());
            }
          }

          os.flush ();
          return r;
        }
      }
    }

    t.print (os, p);
    os.flush ();
    return r;
  }

  bool
  inline_namespace (tree ns)
  {
    if (Decl const* d = decl (ns))
      if (NamespaceDecl const* n = dyn_cast<NamespaceDecl> (d))
        return n->isInline ();

    return false;
  }

  bool
  anonymous_type (tree t)
  {
    QualType qt (any_type (t));

    if (qt.isNull ())
      return false;

    if (TagDecl const* td = qt.getCanonicalType ()->getAsTagDecl ())
      return td->getIdentifier () == 0 && td->getTypedefNameForAnonDecl () == 0;

    return false;
  }

  //
  // Templates.
  //

  static SourceLocation
  default_location ()
  {
    return source_manager->getLocForStartOfFile (main_file_id);
  }

  static SourceLocation
  instantiation_location (location_t l)
  {
    SourceLocation r (source_location (l));
    return r.isValid () ? r : default_location ();
  }

  tree
  instantiate_template (tree t, tree arg)
  {
    Decl const* d (decl (t));
    ClassTemplateDecl const* ct (d != 0 ? dyn_cast<ClassTemplateDecl> (d) : 0);

    if (ct == 0)
      throw failed ();

    QualType at (any_type (arg));

    if (at.isNull ())
      throw failed ();

    SourceLocation loc (default_location ());

    TemplateArgumentListInfo args (loc, loc);
    args.addArgument (
      TemplateArgumentLoc (TemplateArgument (at),
                           ast ().getTrivialTypeSourceInfo (at, loc)));

    QualType r (
      sema_->CheckTemplateIdType (
        ElaboratedTypeKeyword::None,
        TemplateName (const_cast<ClassTemplateDecl*> (ct)),
        loc,
        args,
        0 /* Scope */,
        false /* ForNestedNameSpecifier */));

    if (r.isNull ())
      throw failed ();

    r = r.getCanonicalType ();

    // If we cannot instantiate this type, assume there is no suitable
    // specialization for it.
    //
    int ec (sema_->getDiagnostics ().getNumErrors ());

    if (!sema_->isCompleteType (loc, r) ||
        sema_->getDiagnostics ().getNumErrors () != unsigned (ec))
      return tree ();

    return make (r);
  }

  static VarDecl*
  lookup_var (CXXRecordDecl* rd, char const* name)
  {
    DeclarationName dn (&ast ().Idents.get (name));
    LookupResult r (*sema_, dn, default_location (), Sema::LookupMemberName);
    r.suppressDiagnostics ();

    if (!sema_->LookupQualifiedName (r, rd) || !r.isSingleResult ())
      return 0;

    return dyn_cast<VarDecl> (r.getFoundDecl ()->getUnderlyingDecl ());
  }

  bool
  static_member_value (tree t, char const* name, unsigned long long& v)
  {
    CXXRecordDecl* rd (record_decl (t));

    if (rd == 0 || !rd->hasDefinition ())
      return false;

    VarDecl* var (lookup_var (rd, name));

    if (var == 0)
      return false;

    VarDecl const* def;
    Expr const* init (var->getAnyInitializer (def));

    if (init == 0 &&
        var->getTemplateSpecializationKind () == TSK_ImplicitInstantiation)
    {
      sema_->InstantiateVariableDefinition (default_location (), var);
      init = var->getAnyInitializer (def);
    }

    if (init == 0 || init->isValueDependent ())
      return false;

    Expr::EvalResult er;
    if (!init->EvaluateAsInt (er, ast ()))
      return false;

    llvm::APSInt const& i (er.Val.getInt ());
    v = i.isSigned ()
      ? static_cast<unsigned long long> (i.getSExtValue ())
      : i.getZExtValue ();

    return true;
  }

  bool
  lookup_member_functions (tree t,
                           string const& name,
                           vector<member_function>& fs)
  {
    CXXRecordDecl* rd (record_decl (t));

    if (rd == 0 || !rd->hasDefinition ())
      return false;

    DeclarationName dn (&ast ().Idents.get (name));
    LookupResult r (*sema_, dn, default_location (), Sema::LookupMemberName);
    r.suppressDiagnostics ();

    if (!sema_->LookupQualifiedName (r, rd) || r.isAmbiguous ())
      return false;

    bool found (false);

    for (NamedDecl* nd: r)
    {
      nd = nd->getUnderlyingDecl ();

      if (isa<FunctionTemplateDecl> (nd))
      {
        found = true;
        continue;
      }

      CXXMethodDecl const* m (dyn_cast<CXXMethodDecl> (nd));

      if (m == 0)
        continue;

      found = true;

      member_function f;
      f.decl = m;
      f.static_ = m->isStatic ();
      f.const_ = m->isConst ();
      f.deleted = m->isDeleted ();
      f.access = m->getAccess ();
      f.result = m->getReturnType ();

      for (ParmVarDecl const* p: m->parameters ())
        f.params.push_back (p->getType ());

      fs.push_back (f);
    }

    return found;
  }

  bool
  has_lt_operator (tree t)
  {
    QualType qt (any_type (t));

    if (qt.isNull ())
      return false;

    SourceLocation loc (default_location ());

    // Make sure it is instantiated if it's a class template instantiation.
    //
    if (qt->isRecordType ())
      sema_->isCompleteType (loc, qt);

    Sema::SFINAETrap trap (*sema_);

    QualType ct (qt.getNonReferenceType ().withConst ());
    Expr* x (new (ast ()) OpaqueValueExpr (loc, ct, VK_LValue));
    Expr* y (new (ast ()) OpaqueValueExpr (loc, ct, VK_LValue));

    ExprResult r (sema_->BuildBinOp (0, loc, BO_LT, x, y));
    return !r.isInvalid () && !trap.hasErrorOccurred ();
  }

  bool
  is_friend (tree c, tree f)
  {
    CXXRecordDecl* rd (record_decl (c));
    CXXRecordDecl* fd (record_decl (f));

    if (rd == 0 || fd == 0 || !rd->hasDefinition ())
      return false;

    fd = fd->getCanonicalDecl ();

    for (FriendDecl const* fr: rd->getDefinition ()->friends ())
    {
      if (TypeSourceInfo* ti = fr->getFriendType ())
      {
        if (CXXRecordDecl const* x = ti->getType ()->getAsCXXRecordDecl ())
          if (x->getCanonicalDecl () == fd)
            return true;
      }
      else if (NamedDecl const* nd = fr->getFriendDecl ())
      {
        if (CXXRecordDecl const* x = dyn_cast<CXXRecordDecl> (nd))
          if (x->getCanonicalDecl () == fd)
            return true;
      }
    }

    return false;
  }

  tree
  pointer_to (tree t)
  {
    QualType qt (any_type (t));

    if (qt.isNull ())
      return tree ();

    return make (ast ().getPointerType (qt).getCanonicalType ());
  }

  tree
  class_template_of (tree t)
  {
    QualType qt (any_type (t));

    if (qt.isNull ())
      return tree ();

    TemplateDecl* td (0);

    if (TemplateSpecializationType const* ts =
          qt->getAs<TemplateSpecializationType> ())
      td = ts->getTemplateName ().getAsTemplateDecl ();

    if (td == 0)
    {
      if (ClassTemplateSpecializationDecl const* sd =
            dyn_cast_or_null<ClassTemplateSpecializationDecl> (
              qt->getAsCXXRecordDecl ()))
        td = sd->getSpecializedTemplate ();
    }

    ClassTemplateDecl* ct (dyn_cast_or_null<ClassTemplateDecl> (td));

    if (ct == 0)
      return tree ();

    // Get to the most general template declaration.
    //
    while (ClassTemplateDecl* m = ct->getInstantiatedFromMemberTemplate ())
      ct = m;

    return make (ct);
  }

  string
  qualified_name (tree t)
  {
    Decl const* d (decl (t));

    if (d == 0)
    {
      QualType qt (any_type (t));
      return qt.isNull () ? string () : type_as_string (qt);
    }

    NamedDecl const* nd (dyn_cast<NamedDecl> (d));

    if (nd == 0)
      return string ();

    string r;
    llvm::raw_string_ostream os (r);
    nd->printQualifiedName (os, policy ());
    os.flush ();
    return r;
  }

  //
  // Includes.
  //

  static bool
  make_site (FileID fid, include_site& s)
  {
    SourceManager& m (sm ());
    SourceLocation inc (m.getIncludeLoc (fid));

    if (inc.isInvalid ())
      return false;

    inc = m.getExpansionLoc (inc);

    PresumedLoc p (m.getPresumedLoc (inc));
    if (p.isInvalid ())
      return false;

    if (OptionalFileEntryRef fe = m.getFileEntryRefForID (fid))
      s.included = string (fe->getName ());
    else
      return false;

    s.file = p.getFilename ();
    s.line = p.getLine ();
    s.main = m.getFileID (inc) == main_file_id;
    s.id = inc.getRawEncoding ();
    return true;
  }

  vector<include_site>
  include_sites ()
  {
    vector<include_site> r;
    SourceManager& m (sm ());

    for (unsigned int i (0), n (m.local_sloc_entry_size ()); i != n; ++i)
    {
      SrcMgr::SLocEntry const& e (m.getLocalSLocEntry (i));

      if (!e.isFile ())
        continue;

      FileID fid (m.getFileID (SourceLocation::getFromRawEncoding (
                                 e.getOffset ())));
      include_site s;
      if (make_site (fid, s))
        r.push_back (s);
    }

    return r;
  }

  bool
  included_from (location_t l, include_site& s)
  {
    SourceLocation sl (source_location (l));

    if (sl.isInvalid ())
      return false;

    FileID fid (sm ().getFileID (sl));

    if (fid == main_file_id)
      return false;

    return make_site (fid, s);
  }
}

//
// Node keys.
//

tree
tree_node_key (tree t)
{
  if (t.kind () == tree::type_kind)
    return make (type (t).getCanonicalType ());

  if (t.kind () == tree::type_decl_kind)
    return make (any_type (t).getCanonicalType ());

  return t;
}

tree
tree_hint_key (tree t)
{
  // Name hints are keyed by the typedef declarations.
  //
  QualType qt (type (t));

  if (!qt.isNull ())
  {
    if (qt.hasLocalQualifiers ())
      return tree ();

    if (TypedefType const* tt = dyn_cast<TypedefType> (qt.getTypePtr ()))
      return make (tt->getDecl ());

    if (UsingType const* ut = dyn_cast<UsingType> (qt.getTypePtr ()))
      return tree_hint_key (make (ut->desugar ()));

    return tree ();
  }

  if (Decl const* d = decl (t))
  {
    if (isa<TypedefNameDecl> (d))
      return t;
  }

  return tree ();
}

//
// GCC tree API subset.
//

gcc_tree_code_type
TREE_CODE (tree t)
{
  switch (t.kind ())
  {
  case tree::null_kind:
  case tree::error_kind:
    return ERROR_MARK;
  case tree::int_cst_kind:
    return INTEGER_CST;
  case tree::real_cst_kind:
    return REAL_CST;
  case tree::type_decl_kind:
    return TYPE_DECL;
  case tree::decl_kind:
    {
      Decl const* d (decl (t));

      if (isa<TranslationUnitDecl> (d) ||
          isa<NamespaceDecl> (d) ||
          isa<NamespaceAliasDecl> (d))
        return NAMESPACE_DECL;

      if (isa<TypeDecl> (d))
        return TYPE_DECL;

      if (isa<FieldDecl> (d) || isa<IndirectFieldDecl> (d))
        return FIELD_DECL;

      if (isa<ParmVarDecl> (d))
        return PARM_DECL;

      if (isa<VarDecl> (d))
        return VAR_DECL;

      if (isa<EnumConstantDecl> (d))
        return CONST_DECL;

      if (isa<CXXMethodDecl> (d))
        return BASELINK;

      if (FunctionTemplateDecl const* ft = dyn_cast<FunctionTemplateDecl> (d))
        return isa<CXXMethodDecl> (ft->getTemplatedDecl ())
          ? BASELINK
          : FUNCTION_DECL;

      if (isa<FunctionDecl> (d))
        return FUNCTION_DECL;

      if (isa<TemplateDecl> (d))
        return TEMPLATE_DECL;

      if (isa<UsingDecl> (d))
        return USING_DECL;

      return LAST_DECL_CODE;
    }
  case tree::type_kind:
    {
      QualType ct (type (t).getCanonicalType ());
      Type const* tp (ct.getTypePtr ());

      switch (tp->getTypeClass ())
      {
      case Type::Builtin:
        {
          BuiltinType const* b (cast<BuiltinType> (tp));

          if (b->getKind () == BuiltinType::Void)
            return VOID_TYPE;

          if (b->getKind () == BuiltinType::Bool)
            return BOOLEAN_TYPE;

          if (b->getKind () == BuiltinType::NullPtr)
            return NULLPTR_TYPE;

          if (b->isInteger ())
            return INTEGER_TYPE;

          if (b->isFloatingPoint ())
            return REAL_TYPE;

          return LANG_TYPE;
        }
      case Type::Record:
        return tp->isUnionType () ? UNION_TYPE : RECORD_TYPE;
      case Type::InjectedClassName:
        return RECORD_TYPE;
      case Type::Enum:
        return ENUMERAL_TYPE;
      case Type::Pointer:
      case Type::BlockPointer:
      case Type::ObjCObjectPointer:
        return POINTER_TYPE;
      case Type::LValueReference:
      case Type::RValueReference:
        return REFERENCE_TYPE;
      case Type::MemberPointer:
        return OFFSET_TYPE;
      case Type::ConstantArray:
      case Type::IncompleteArray:
      case Type::VariableArray:
      case Type::DependentSizedArray:
        return ARRAY_TYPE;
      case Type::FunctionProto:
      case Type::FunctionNoProto:
        return FUNCTION_TYPE;
      case Type::Complex:
        return COMPLEX_TYPE;
      case Type::Vector:
      case Type::ExtVector:
        return VECTOR_TYPE;
      case Type::TemplateTypeParm:
        return TEMPLATE_TYPE_PARM;
      case Type::DependentName:
        return TYPENAME_TYPE;
      case Type::Decltype:
        return DECLTYPE_TYPE;
      default:
        return LANG_TYPE;
      }
    }
  }

  return ERROR_MARK;
}

tree_code_class
TREE_CODE_CLASS (gcc_tree_code_type c)
{
  if (c > IDENTIFIER_NODE && c < LAST_DECL_CODE + 1)
    return tcc_declaration;

  if (c > LAST_DECL_CODE && c < INTEGER_CST)
    return tcc_type;

  if (c == INTEGER_CST || c == REAL_CST)
    return tcc_constant;

  return tcc_exceptional;
}

char const*
gcc_tree_code_name (gcc_tree_code_type c)
{
  static char const* names[] = {
    "error_mark",
    "identifier_node",
    "namespace_decl",
    "type_decl",
    "field_decl",
    "var_decl",
    "const_decl",
    "parm_decl",
    "function_decl",
    "template_decl",
    "using_decl",
    "baselink",
    "decl",
    "record_type",
    "union_type",
    "enumeral_type",
    "void_type",
    "boolean_type",
    "integer_type",
    "real_type",
    "complex_type",
    "vector_type",
    "nullptr_type",
    "pointer_type",
    "reference_type",
    "offset_type",
    "array_type",
    "function_type",
    "method_type",
    "template_type_parm",
    "typename_type",
    "decltype_type",
    "lang_type",
    "integer_cst",
    "real_cst"};

  return names[c];
}

tree
TREE_TYPE (tree t)
{
  switch (t.kind ())
  {
  case tree::type_decl_kind:
    return make (any_type (t));
  case tree::int_cst_kind:
  case tree::real_cst_kind:
    return tree (); // Use the constant's type directly.
  case tree::decl_kind:
    {
      Decl const* d (decl (t));

      if (TypedefNameDecl const* td = dyn_cast<TypedefNameDecl> (d))
        return make (ast ().getTypedefType (ElaboratedTypeKeyword::None,
                                            std::nullopt,
                                            td));

      if (TagDecl const* td = dyn_cast<TagDecl> (d))
        return make (QualType (ast ().getCanonicalTagType (td)));

      if (TypeDecl const* td = dyn_cast<TypeDecl> (d))
        return make (ast ().getTypeDeclType (td));

      if (ValueDecl const* vd = dyn_cast<ValueDecl> (d))
        return make (vd->getType ());

      return tree ();
    }
  case tree::type_kind:
    {
      QualType qt (type (t));
      QualType ct (qt.getCanonicalType ());

      if (PointerType const* p = qt->getAs<PointerType> ())
        return make (p->getPointeeType ());

      if (ReferenceType const* r = qt->getAs<ReferenceType> ())
        return make (r->getPointeeType ());

      if (ArrayType const* a = ast ().getAsArrayType (qt))
        return make (a->getElementType ());

      if (FunctionType const* f = qt->getAs<FunctionType> ())
        return make (f->getReturnType ());

      if (MemberPointerType const* m = qt->getAs<MemberPointerType> ())
        return make (m->getPointeeType ());

      (void) ct;
      return tree ();
    }
  default:
    return tree ();
  }
}

tree
TYPE_MAIN_VARIANT (tree t)
{
  QualType qt (any_type (t));

  if (qt.isNull ())
    return t;

  return make (qt.getCanonicalType ().getUnqualifiedType ());
}

tree
TYPE_NAME (tree t)
{
  QualType qt (any_type (t));

  if (qt.isNull ())
    return tree ();

  if (t.kind () == tree::type_kind && !qt.hasLocalQualifiers ())
  {
    if (TypedefType const* tt = dyn_cast<TypedefType> (qt.getTypePtr ()))
      return make (tt->getDecl ());
  }

  QualType ct (qt.getCanonicalType ());

  if (TagDecl const* td = ct->getAsTagDecl ())
    return make (td);

  if (isa<BuiltinType> (ct.getTypePtr ()))
    return make_type_decl (ct.getUnqualifiedType ());

  return tree ();
}

bool
CLASS_TYPE_P (tree t)
{
  if (t.kind () != tree::type_kind)
    return false;

  return type (t).getCanonicalType ()->getAsCXXRecordDecl () != 0;
}

bool
COMPLETE_TYPE_P (tree t)
{
  QualType qt (any_type (t));

  if (qt.isNull ())
    return false;

  return !qt.getCanonicalType ()->isIncompleteType ();
}

bool
CLASSTYPE_TEMPLATE_INSTANTIATION (tree t)
{
  CXXRecordDecl* r (record_decl (t));

  if (r == 0)
    return false;

  if (ClassTemplateSpecializationDecl const* s =
      dyn_cast<ClassTemplateSpecializationDecl> (r))
    return s->getSpecializationKind () != TSK_ExplicitSpecialization;

  return false;
}

bool
DECL_CLASS_TEMPLATE_P (tree t)
{
  Decl const* d (decl (t));
  return d != 0 && isa<ClassTemplateDecl> (d);
}

bool
TYPE_UNSIGNED (tree t)
{
  QualType qt (any_type (t));
  return !qt.isNull () && qt.getCanonicalType ()->isUnsignedIntegerOrEnumerationType ();
}

bool
TYPE_POLYMORPHIC_P (tree t)
{
  CXXRecordDecl* r (record_decl (t));
  return r != 0 && r->hasDefinition () && r->isPolymorphic ();
}

bool
CP_TYPE_CONST_P (tree t)
{
  QualType qt (any_type (t));
  return !qt.isNull () && qt.isConstQualified ();
}

bool
CP_TYPE_VOLATILE_P (tree t)
{
  QualType qt (any_type (t));
  return !qt.isNull () && qt.isVolatileQualified ();
}

bool
CP_TYPE_RESTRICT_P (tree t)
{
  QualType qt (any_type (t));
  return !qt.isNull () && qt.isRestrictQualified ();
}

tree
CP_DECL_CONTEXT (tree t)
{
  if (Decl const* d = decl (t))
  {
    if (isa<TranslationUnitDecl> (d))
      return global_namespace;

    return make_scope (d->getDeclContext ());
  }

  tree n (TYPE_NAME (t));

  if (Decl const* d = decl (n))
    return make_scope (d->getDeclContext ());

  return global_namespace;
}

tree
DECL_ORIGINAL_TYPE (tree t)
{
  if (Decl const* d = decl (t))
  {
    if (TypedefNameDecl const* td = dyn_cast<TypedefNameDecl> (d))
      return make (td->getUnderlyingType ());
  }

  return tree ();
}

tree
ORIGINAL_NAMESPACE (tree t)
{
  if (Decl const* d = decl (t))
  {
    if (NamespaceAliasDecl const* a = dyn_cast<NamespaceAliasDecl> (d))
      return make (a->getNamespace ());
  }

  return t;
}

string
DECL_NAME (tree t)
{
  if (Decl const* d = decl (t))
  {
    if (NamedDecl const* n = dyn_cast<NamedDecl> (d))
      return n->getNameAsString ();
  }
  else if (t.kind () == tree::type_decl_kind)
    return type_as_string (any_type (t));

  return string ();
}

location_t
real_source_location (tree t)
{
  if (Decl const* d = decl (t))
    return make_location (d->getLocation ());

  return location_t ();
}

unsigned long long
integer_value (tree t)
{
  if (t.kind () == tree::int_cst_kind)
    return static_cast<tree_int_cst const*> (t.ptr ())->value;

  return 0;
}

// Parse the canonical fundamental type name (as returned by
// lookup::parse_fundamental(), e.g., "short unsigned int").
//
static QualType
fundamental_type (string const& n)
{
  ASTContext& c (ast ());

  bool si (false), un (false), sh (false), lo (false), ll (false);
  string t;

  istringstream is (n);
  for (string w; is >> w;)
  {
    if (w == "signed") si = true;
    else if (w == "unsigned") un = true;
    else if (w == "short") sh = true;
    else if (w == "long")
    {
      if (lo) {lo = false; ll = true;} else lo = true;
    }
    else if (t.empty ()) t = w;
    else return QualType ();
  }

  if (t.empty ())
  {
    if (!(si || un || sh || lo || ll))
      return QualType ();

    t = "int";
  }

  if (t == "int")
  {
    if (sh) return un ? c.UnsignedShortTy : c.ShortTy;
    if (lo) return un ? c.UnsignedLongTy : c.LongTy;
    if (ll) return un ? c.UnsignedLongLongTy : c.LongLongTy;
    return un ? c.UnsignedIntTy : c.IntTy;
  }

  if (sh || ll)
    return QualType ();

  if (t == "char")
  {
    if (lo) return QualType ();
    if (si) return c.SignedCharTy;
    if (un) return c.UnsignedCharTy;
    return c.CharTy;
  }

  if (si || un)
    return QualType ();

  if (t == "double")
    return lo ? c.LongDoubleTy : c.DoubleTy;

  if (lo)
    return QualType ();

  if (t == "bool") return c.BoolTy;
  if (t == "float") return c.FloatTy;
  if (t == "wchar_t") return c.WCharTy;
  if (t == "char16_t") return c.Char16Ty;
  if (t == "char32_t") return c.Char32Ty;
  if (t == "char8_t") return c.Char8Ty;
  if (t == "void") return c.VoidTy;

  return QualType ();
}

tree
lookup_qualified_name (tree scope, string const& name, bool type, bool)
{
  if (name.empty ())
    return error_mark_node;

  // Fundamental types.
  //
  if (scope == global_namespace)
  {
    char f (name[0]);
    if (f == 's' || f == 'u' || f == 'l' || f == 'i' || f == 'c' ||
        f == 'b' || f == 'f' || f == 'd' || f == 'w' || f == 'v')
    {
      QualType t (fundamental_type (name));
      if (!t.isNull ())
        return make_type_decl (t);
    }
  }

  DeclContext* dc (decl_context (scope));

  if (dc == 0)
    return error_mark_node;

  // Make sure the class is complete (e.g., template instantiation).
  //
  if (TagDecl* td = dyn_cast<TagDecl> (dc))
  {
    if (!td->isCompleteDefinition ())
    {
      QualType t (ast ().getCanonicalTagType (td));
      sema_->isCompleteType (source_manager->getLocForStartOfFile (main_file_id),
                            t);

      if (TagDecl* def = td->getDefinition ())
        dc = def;
      else
        return error_mark_node;
    }
  }

  DeclarationName dn (&ast ().Idents.get (name));
  LookupResult r (*sema_, dn, SourceLocation (), Sema::LookupOrdinaryName);
  r.suppressDiagnostics ();

  if (!sema_->LookupQualifiedName (r, dc))
    return error_mark_node;

  NamedDecl* func (0);
  NamedDecl* other (0);

  for (NamedDecl* nd: r)
  {
    nd = nd->getUnderlyingDecl ();

    if (type)
    {
      if (isa<TypeDecl> (nd) ||
          isa<NamespaceDecl> (nd) ||
          isa<NamespaceAliasDecl> (nd) ||
          isa<ClassTemplateDecl> (nd) ||
          isa<TypeAliasTemplateDecl> (nd))
        return make (nd);

      continue;
    }

    if (isa<FunctionDecl> (nd) || isa<FunctionTemplateDecl> (nd))
    {
      if (func == 0)
        func = nd;
    }
    else if (other == 0)
      other = nd;
  }

  if (type)
    return error_mark_node;

  // Prefer non-functions (e.g., a data member hiding nothing).
  //
  if (other != 0)
    return make (other);

  if (func != 0)
    return make (func);

  return error_mark_node;
}

tree
instantiate_class_template (tree t, location_t l)
{
  QualType qt (any_type (t));

  if (qt.isNull ())
    return error_mark_node;

  int ec (sema_->getDiagnostics ().getNumErrors ());

  if (!sema_->isCompleteType (instantiation_location (l),
                             qt.getCanonicalType ()) ||
      sema_->getDiagnostics ().getNumErrors () != unsigned (ec))
    return error_mark_node;

  return t;
}

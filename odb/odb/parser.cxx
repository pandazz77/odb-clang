// file      : odb/parser.cxx
// license   : GNU GPL v3; see accompanying LICENSE file

#include <odb/clang.hxx>

#include <set>
#include <map>
#include <vector>
#include <string>
#include <cassert>
#include <sstream>
#include <iostream>

#include <odb/diagnostics.hxx>
#include <odb/parser.hxx>
#include <odb/semantics.hxx>

using namespace std;
using namespace semantics;

namespace cl = clang;

// Build the semantic graph from the Clang AST. The structure follows the
// original GCC tree-based implementation: namespace-level declarations
// are collected and traversed in the source code order, interleaved with
// position pragmas.
//
class parser::impl
{
public:
  typedef parser::failed failed;

  impl (options const&, loc_pragmas&, ns_loc_pragmas&, decl_pragmas&);

  unique_ptr<unit>
  parse (tree global_scope, path const& main_file);

private:
  typedef semantics::access access;

  // Extended declaration that is either a real declaration, a virtual
  // declaration, or a pragma. If it is a pragma, then the assoc flag
  // indicated whether this pragma has been associated with a declaration.
  // Otherwise, the assoc flag indicates whether pragmas have been
  // associated with this declaration (we use this to ignore certain
  // declarations for pragma association purposes, e.g., the anonymous
  // type in struct {...} m_).
  //
  struct tree_decl
  {
    tree decl;
    virt_declaration const* vdecl;
    pragma const* prag;
    mutable bool assoc; // Allow modification via std::set iterator.

    tree_decl (tree d): decl (d), vdecl (0), prag (0), assoc (false) {}
    tree_decl (virt_declaration const& d)
        : decl (0), vdecl (&d), prag (0), assoc (false) {}
    tree_decl (pragma const& p)
        : decl (0), vdecl (0), prag (&p), assoc (false) {}

    bool
    operator< (tree_decl const& y) const;
  };

  typedef multiset<tree_decl> decl_set;

private:
  void
  collect (cl::DeclContext const*);

  void
  collect_members (cl::CXXRecordDecl const*, decl_set&, bool templates_only);

  void
  emit ();

  // Emit a type declaration. This is either a named class-type definition/
  // declaration or a typedef. In the former case the function returns the
  // newly created type node. In the latter case it returns 0.
  //
  type*
  emit_type_decl (cl::Decl const*);

  // Emit a template declaration.
  //
  void
  emit_template_decl (cl::ClassTemplateDecl const*);

  class_template&
  emit_class_template (cl::ClassTemplateDecl const*, bool stub = false);

  union_template&
  emit_union_template (cl::ClassTemplateDecl const*, bool stub = false);

  template <typename T>
  T&
  emit_class (tree, path const& f, size_t l, size_t c, bool stub = false);

  template <typename T>
  T&
  emit_union (tree, path const& f, size_t l, size_t c, bool stub = false);

  // Access is not used when creating a stub.
  //
  enum_&
  emit_enum (tree,
             access,
             path const& f,
             size_t l,
             size_t c,
             bool stub = false);

  // Create new or find existing semantic graph type.
  //
  type&
  emit_type (tree, access, path const& f, size_t l, size_t c);

  type&
  create_type (tree, access, path const& f, size_t l, size_t c);

  void
  emit_member (cl::FieldDecl const*,
               scope&,
               char const* kind,
               decl_set::const_iterator b,
               decl_set::const_iterator i,
               decl_set::const_iterator e);

  string
  emit_type_name (tree);

  //
  // Pragma handling.
  //
  void
  add_pragma (node&, pragma const&);

  // Process positioned and named pragmas.
  //
  void
  process_pragmas (declaration const&,
                   node&,
                   string const& name,
                   decl_set::const_iterator begin,
                   decl_set::const_iterator cur,
                   decl_set::const_iterator end);

  // Process named pragmas only.
  //
  void
  process_named_pragmas (declaration const&, node&);

  void
  diagnose_unassoc_pragmas (decl_set const&);

  // Return declaration's enclosing non-inline namespaces, outermost first.
  //
  static void
  ns_chain (cl::Decl const*, vector<cl::NamespaceDecl const*>&);

  // Return declaration's access.
  //
  static access
  decl_access (cl::Decl const* d)
  {
    switch (d->getAccess ())
    {
    case cl::AS_private:   return access::private_;
    case cl::AS_protected: return access::protected_;
    default:               return access::public_;
    }
  }

  static location_t
  dloc (cl::Decl const* d)
  {
    return odbc::make_location (d->getLocation ());
  }

  //
  //
  template <typename T>
  void
  define_fund (cl::QualType, char const* name);

private:
  options const& ops_;
  loc_pragmas& loc_pragmas_;
  ns_loc_pragmas& ns_loc_pragmas_;
  decl_pragmas& decl_pragmas_;

  bool trace;
  ostream& ts;

  unit* unit_;
  scope* scope_;
  vector<scope*> class_scopes_; // Current hierarchy of class-like scopes.
  size_t error_;

  decl_set decls_;

  typedef map<location_t, tree> decl_map;
  decl_map all_decls_;

  set<cl::NamespaceDecl const*> namespaces_; // Collected namespaces.
};

bool parser::impl::tree_decl::
operator< (tree_decl const& y) const
{
  location_t xl, yl;
  int xb (0), yb (0);

  if (decl)
    xl = real_source_location (decl);
  else if (vdecl != 0)
  {
    xl = vdecl->ord;
    xb = vdecl->ord_bias;
  }
  else
    xl = prag->loc;

  if (y.decl)
    yl = real_source_location (y.decl);
  else if (y.vdecl != 0)
  {
    yl = y.vdecl->ord;
    yb = y.vdecl->ord_bias;
  }
  else
    yl = y.prag->loc;

  // If both are virtual and at the same location, use the definition
  // location to order them.
  //
  if (vdecl != 0 && y.vdecl != 0 && xl == yl && xb == yb)
  {
    xl = vdecl->loc;
    yl = y.vdecl->loc;
  }

  return xl < yl || (xl == yl && xb < yb);
}

//
// Helpers.
//

namespace
{
  // Return true if the declaration comes from the compiler itself (built-
  // in declarations, predefined macros buffer, etc).
  //
  bool
  builtin_p (cl::Decl const* d)
  {
    if (d->isImplicit ())
      return true;

    cl::SourceLocation l (d->getLocation ());

    if (l.isInvalid ())
      return true;

    cl::SourceManager& sm (odbc::sm ());
    l = sm.getExpansionLoc (l);

    return sm.isWrittenInBuiltinFile (l) || sm.isWrittenInCommandLineFile (l);
  }

  // Return true if this tag declaration is the one that represents the
  // type (the definition, if any, otherwise the first declaration).
  //
  bool
  primary_tag (cl::TagDecl const* t)
  {
    if (t->isImplicit () || cl::isa<cl::ClassTemplateSpecializationDecl> (t))
      return false;

    if (cl::CXXRecordDecl const* r = cl::dyn_cast<cl::CXXRecordDecl> (t))
    {
      if (r->isInjectedClassName () ||
          r->isLambda () ||
          r->getDescribedClassTemplate () != 0)
        return false;
    }

    if (cl::TagDecl const* def = t->getDefinition ())
      return def == t;

    return t == t->getCanonicalDecl ();
  }

  bool
  primary_template (cl::ClassTemplateDecl const* t)
  {
    if (t->isImplicit ())
      return false;

    cl::CXXRecordDecl const* r (t->getTemplatedDecl ());

    if (cl::CXXRecordDecl const* def = r->getDefinition ())
      return def == r;

    return t == t->getCanonicalDecl ();
  }

  inline tree
  tag_type (cl::TagDecl const* t)
  {
    return odbc::make (cl::QualType (odbc::ast ().getCanonicalTagType (t)));
  }

  inline tree
  template_key (cl::ClassTemplateDecl const* t)
  {
    return odbc::make (t->getCanonicalDecl ());
  }

  // Return the type with all the qualifiers (including those on array
  // elements) and sugar removed. Return the qualifiers in quals.
  //
  cl::QualType
  main_variant (cl::QualType t, cl::Qualifiers& quals)
  {
    return odbc::ast ().getUnqualifiedArrayType (t.getCanonicalType (), quals);
  }

  inline cl::QualType
  main_variant (cl::QualType t)
  {
    cl::Qualifiers q;
    return main_variant (t, q);
  }

  inline cl::QualType
  any_type (tree t)
  {
    if (t.kind () == tree::type_decl_kind)
      t = TREE_TYPE (t);

    return odbc::type (t);
  }
}

//
// Function templates.
//

template <typename T>
void parser::impl::
define_fund (cl::QualType qt, char const* name)
{
  tree t (odbc::make (qt.getCanonicalType ()));

  T& node (unit_->new_fund_node<T> (t));
  unit_->new_edge<defines> (*scope_, node, name);
  unit_->insert (t, node);

  process_named_pragmas (t, node);
}

template <typename T>
T& parser::impl::
emit_class (tree c, path const& file, size_t line, size_t clmn, bool stub)
{
  c = TYPE_MAIN_VARIANT (c);

  // See if there is a stub already for this type.
  //
  T* c_node (0);

  if (node* n = unit_->find (c))
  {
    c_node = &dynamic_cast<T&> (*n);
  }
  else
  {
    c_node = &unit_->new_node<T> (file, line, clmn, c);
    unit_->insert (c, *c_node);
  }

  cl::CXXRecordDecl const* rd (odbc::record_decl (c));

  if (stub || !COMPLETE_TYPE_P (c) || rd == 0 || !rd->hasDefinition ())
    return *c_node;

  rd = rd->getDefinition ();

  // Note: "include" the base classes into the class scope (see comment for
  // self-typedefs in emit_type_decl()).
  //
  class_scopes_.push_back (c_node);

  // Traverse base information.
  //
  for (cl::CXXBaseSpecifier const& bs: rd->bases ())
  {
    access a (access::public_);

    switch (bs.getAccessSpecifier ())
    {
    case cl::AS_protected: a = access::protected_; break;
    case cl::AS_private:   a = access::private_; break;
    default:               break;
    }

    bool virt (bs.isVirtual ());
    tree base (odbc::make (main_variant (bs.getType ())));

    // Find the corresponding graph node. If we cannot find one then
    // the base is a template instantiation since an ordinary class
    // has to be defined (complete) in order to be a base.
    //
    class_* b_node (0);
    string name;

    if (node* n = unit_->find (base))
    {
      b_node = &dynamic_cast<class_&> (*n);

      if (trace)
        name = DECL_NAME (TYPE_NAME (base));
    }
    else
    {
      // Use public access for a template instantiation in the inheritance
      // declaration.
      //
      b_node = &dynamic_cast<class_&> (
        emit_type (base, access::public_, file, line, clmn));

      if (trace)
        name = emit_type_name (base);
    }

    unit_->new_edge<inherits> (*c_node, *b_node, a, virt);

    if (trace)
      ts << "\t" << a.string () << (virt ? " virtual" : "") << " base "
         << name << " (" << static_cast<type*> (b_node) << ")" << endl;
  }

  // Collect member declarations so that we can traverse them in
  // the source code order.
  //
  decl_set decls;
  collect_members (rd, decls, false);

  // Add virtual declarations if any.
  //
  {
    virt_declarations::const_iterator i (virt_declarations_.find (c));

    if (i != virt_declarations_.end ())
      decls.insert (i->second.begin (), i->second.end ());
  }

  // Add location pragmas if any.
  //
  {
    loc_pragmas::const_iterator i (loc_pragmas_.find (c));

    if (i != loc_pragmas_.end ())
      decls.insert (i->second.begin (), i->second.end ());
  }

  scope* prev_scope (scope_);
  scope_ = c_node;

  for (decl_set::const_iterator b (decls.begin ()), i (b), e (decls.end ());
       i != e; ++i)
  {
    // Skip pragmas.
    //
    if (i->prag != 0)
      continue;

    // Handle virtual declarations.
    //
    if (i->vdecl != 0)
    {
      virt_declaration const& vd (*i->vdecl);

      switch (vd.tree_code)
      {
      case FIELD_DECL:
        {
          // First check that it doesn't conflict with any of the real
          // data members defined in this class.
          //
          tree d (lookup_qualified_name (c, vd.name, false, false));

          if (d != error_mark_node && TREE_CODE (d) == FIELD_DECL)
          {
            error (vd.loc) << "virtual data member declaration '" << vd.name
                           << "' conflicts with a previous declaration"
                           << endl;

            location_t l (real_source_location (d));
            info (l) << "'" << vd.name << "' was previously declared here"
                     << endl;

            throw failed ();
          }

          path file (LOCATION_FILE (vd.loc));
          size_t line (LOCATION_LINE (vd.loc));
          size_t clmn (LOCATION_COLUMN (vd.loc));

          access a (access::public_);

          type& type_node (emit_type (vd.type, a, file, line, clmn));
          data_member& member_node (
            unit_->new_node<data_member> (file, line, clmn, tree (0)));

          unit_->new_edge<names> (*c_node, member_node, vd.name, a);
          belongs& edge (unit_->new_edge<belongs> (member_node, type_node));

          // See if there is a name hint for this type.
          //
          if (names* hint = unit_->find_hint (vd.type))
            edge.hint (*hint);

          // Process pragmas that may be associated with this field.
          //
          process_pragmas (vd, member_node, vd.name, b, i, e);
          break;
        }
      default:
        {
          assert (false);
          break;
        }
      }
      continue;
    }

    cl::Decl const* d (odbc::decl (i->decl));

    if (cl::isa<cl::TagDecl> (d) || cl::isa<cl::TypedefNameDecl> (d))
    {
      type* n (emit_type_decl (d));

      // If this is a named class-type definition, then handle
      // the pragmas.
      //
      if (n != 0)
        process_pragmas (n->tree_node (), *n, n->name (), b, i, e);
    }
    else if (cl::ClassTemplateDecl const* t =
               cl::dyn_cast<cl::ClassTemplateDecl> (d))
      emit_template_decl (t);
    else if (cl::FieldDecl const* f = cl::dyn_cast<cl::FieldDecl> (d))
      emit_member (f, *c_node, "data member", b, i, e);
  }

  // Diagnose any position pragmas that haven't been associated.
  //
  diagnose_unassoc_pragmas (decls);

  scope_ = prev_scope;
  class_scopes_.pop_back ();

  return *c_node;
}

template <typename T>
T& parser::impl::
emit_union (tree u, path const& file, size_t line, size_t clmn, bool stub)
{
  u = TYPE_MAIN_VARIANT (u);

  // See if there is a stub already for this type.
  //
  T* u_node (0);

  if (node* n = unit_->find (u))
  {
    u_node = &dynamic_cast<T&> (*n);
  }
  else
  {
    u_node = &unit_->new_node<T> (file, line, clmn, u);
    unit_->insert (u, *u_node);
  }

  cl::CXXRecordDecl const* rd (odbc::record_decl (u));

  if (stub || !COMPLETE_TYPE_P (u) || rd == 0 || !rd->hasDefinition ())
    return *u_node;

  rd = rd->getDefinition ();

  class_scopes_.push_back (u_node);

  // Collect member declarations so that we can traverse them in
  // the source code order.
  //
  decl_set decls;
  collect_members (rd, decls, false);

  // Add location pragmas if any.
  //
  {
    loc_pragmas::const_iterator i (loc_pragmas_.find (u));

    if (i != loc_pragmas_.end ())
      decls.insert (i->second.begin (), i->second.end ());
  }

  scope* prev_scope (scope_);
  scope_ = u_node;

  for (decl_set::const_iterator b (decls.begin ()), i (b), e (decls.end ());
       i != e; ++i)
  {
    // Skip pragmas.
    //
    if (i->prag)
      continue;

    cl::Decl const* d (odbc::decl (i->decl));

    if (cl::isa<cl::TagDecl> (d) || cl::isa<cl::TypedefNameDecl> (d))
    {
      type* n (emit_type_decl (d));

      // If this is a named class-type definition, then handle
      // the pragmas.
      //
      if (n != 0)
        process_pragmas (n->tree_node (), *n, n->name (), b, i, e);
    }
    else if (cl::ClassTemplateDecl const* t =
               cl::dyn_cast<cl::ClassTemplateDecl> (d))
      emit_template_decl (t);
    else if (cl::FieldDecl const* f = cl::dyn_cast<cl::FieldDecl> (d))
      emit_member (f, *u_node, "union member", b, i, e);
  }

  // Diagnose any position pragmas that haven't been associated.
  //
  diagnose_unassoc_pragmas (decls);

  scope_ = prev_scope;
  class_scopes_.pop_back ();
  return *u_node;
}

//
// Functions.
//

parser::impl::
impl (options const& ops,
      loc_pragmas & lp,
      ns_loc_pragmas& nslp,
      decl_pragmas& dp)
    : ops_ (ops),
      loc_pragmas_ (lp),
      ns_loc_pragmas_ (nslp),
      decl_pragmas_ (dp),
      trace (ops.trace ()),
      ts (cerr)
{
}

unique_ptr<unit> parser::impl::
parse (tree, path const& main_file)
{
  unique_ptr<unit> u (new unit (main_file));
  u->insert (global_namespace, *u);
  process_named_pragmas (global_namespace, *u);

  unit_ = u.get ();
  scope_ = unit_;
  error_ = 0;

  // Define fundamental types. Note that we use the GCC names for
  // compatibility (e.g., database type maps rely on them).
  //
  cl::ASTContext& c (odbc::ast ());

  define_fund<fund_void> (c.VoidTy, "void");
  define_fund<fund_bool> (c.BoolTy, "bool");
  define_fund<fund_char> (c.CharTy, "char");
  define_fund<fund_wchar> (c.WCharTy, "wchar_t");

  if (ops_.std () >= cxx_version::cxx11)
  {
    define_fund<fund_char16> (c.Char16Ty, "char16_t");
    define_fund<fund_char32> (c.Char32Ty, "char32_t");
  }

  define_fund<fund_signed_char> (c.SignedCharTy, "signed char");
  define_fund<fund_unsigned_char> (c.UnsignedCharTy, "unsigned char");
  define_fund<fund_short> (c.ShortTy, "short int");
  define_fund<fund_unsigned_short> (c.UnsignedShortTy, "short unsigned int");
  define_fund<fund_int> (c.IntTy, "int");
  define_fund<fund_unsigned_int> (c.UnsignedIntTy, "unsigned int");
  define_fund<fund_long> (c.LongTy, "long int");
  define_fund<fund_unsigned_long> (c.UnsignedLongTy, "long unsigned int");
  define_fund<fund_long_long> (c.LongLongTy, "long long int");
  define_fund<fund_unsigned_long_long> (c.UnsignedLongLongTy,
                                        "long long unsigned int");
  define_fund<fund_float> (c.FloatTy, "float");
  define_fund<fund_double> (c.DoubleTy, "double");
  define_fund<fund_long_double> (c.LongDoubleTy, "long double");

  // First collect all the namespace-level declarations we are
  // interested in in the line-decl map so that they appear in
  // the source code order.
  //
  collect (c.getTranslationUnitDecl ());

  // Add namespace-level position pragmas if any.
  //
  {
    loc_pragmas::const_iterator i (loc_pragmas_.find (global_namespace));

    if (i != loc_pragmas_.end ())
      decls_.insert (i->second.begin (), i->second.end ());
  }

  // Convert position namespace pragmas to name pragmas.
  //
  for (ns_loc_pragmas::const_iterator i (ns_loc_pragmas_.begin ());
       i != ns_loc_pragmas_.end (); ++i)
  {
    pragma const& p (i->pragma);

    decl_map::const_iterator j (all_decls_.lower_bound (p.loc));

    if (j == all_decls_.end ())
    {
      error (p.loc)
        << "db pragma '" << p.pragma_name << "' is not associated with a "
        << "namespace declaration" << endl;
      error_++;
      continue;
    }

    // Find the "namespace difference" between this declaration and
    // the pragma's namespace. The outermost namespace in the result
    // is what we are looking for.
    //
    tree ns (0);

    for (tree prev (j->second), scope (CP_DECL_CONTEXT (prev));;
         scope = CP_DECL_CONTEXT (scope))
    {
      if (scope == i->ns)
      {
        ns = prev;
        break;
      }

      if (scope == global_namespace || !scope)
        break;

      prev = scope;
    }

    if (ns == 0 || TREE_CODE (ns) != NAMESPACE_DECL)
    {
      error (p.loc)
        << "db pragma '" << p.pragma_name << "' is not associated with a "
        << "namespace declaration" << endl;
      error_++;
      continue;
    }

    pragma_set& s (decl_pragmas_[ns]);
    pragma_set::iterator it (s.find (p.context_name));

    // Make sure we override only if this pragma came after the one
    // already in the set.
    //
    if (it == s.end () || it->second.loc <= p.loc)
      s.insert (p);
  }

  // Construct the semantic graph.
  //
  if (error_ == 0)
    emit ();

  if (error_ > 0)
    throw failed ();

  return u;
}

void parser::impl::
collect (cl::DeclContext const* dc)
{
  for (cl::Decl const* d: dc->decls ())
  {
    // Transparent contexts (extern "C", export).
    //
    if (cl::isa<cl::LinkageSpecDecl> (d) || cl::isa<cl::ExportDecl> (d))
    {
      collect (cl::cast<cl::DeclContext> (d));
      continue;
    }

    if (cl::NamespaceDecl const* ns = cl::dyn_cast<cl::NamespaceDecl> (d))
    {
      if (d->getLocation ().isValid ())
        all_decls_[dloc (d)] = odbc::make (d);

      // Skip anonymous namespaces (there could be nothing of interest to us
      // inside but they wreck havoc with our attempts to sort declarations
      // into namespaces).
      //
      if (ns->isAnonymousNamespace ())
        continue;

      ns = ns->getCanonicalDecl ();

      if (!namespaces_.insert (ns).second)
        continue; // Already collected (re-opened namespace).

      if (trace)
        ts << "namespace " << ns->getName ().str () << " at "
           << DECL_SOURCE_FILE (odbc::make (ns)) << ":"
           << DECL_SOURCE_LINE (odbc::make (ns)) << endl;

      for (cl::NamespaceDecl const* r: ns->redecls ())
        collect (r);

      continue;
    }

    if (builtin_p (d))
      continue;

    if (cl::isa<cl::NamedDecl> (d))
      all_decls_[dloc (d)] = odbc::make (d);

    if (cl::TagDecl const* t = cl::dyn_cast<cl::TagDecl> (d))
    {
      if (primary_tag (t))
        decls_.insert (odbc::make (t));
    }
    else if (cl::isa<cl::TypedefNameDecl> (d))
      decls_.insert (odbc::make (d));
    else if (cl::ClassTemplateDecl const* t =
               cl::dyn_cast<cl::ClassTemplateDecl> (d))
    {
      if (primary_template (t))
        decls_.insert (odbc::make (t));
    }
  }
}

void parser::impl::
collect_members (cl::CXXRecordDecl const* rd,
                 decl_set& decls,
                 bool templates_only)
{
  for (cl::Decl const* d: rd->decls ())
  {
    if (cl::ClassTemplateDecl const* t =
          cl::dyn_cast<cl::ClassTemplateDecl> (d))
    {
      if (primary_template (t))
        decls.insert (odbc::make (t));
    }

    if (templates_only)
      continue;

    if (cl::TagDecl const* t = cl::dyn_cast<cl::TagDecl> (d))
    {
      if (primary_tag (t))
        decls.insert (odbc::make (t));
    }
    else if (cl::isa<cl::TypedefNameDecl> (d))
    {
      if (!d->isImplicit ())
        decls.insert (odbc::make (d));
    }
    else if (cl::isa<cl::FieldDecl> (d))
    {
      if (!d->isImplicit ())
        decls.insert (odbc::make (d));
    }
  }
}

void parser::impl::
emit ()
{
  for (decl_set::const_iterator b (decls_.begin ()), i (b),
         e (decls_.end ()); i != e; ++i)
  {
    // Skip pragmas.
    //
    if (i->prag)
      continue;

    cl::Decl const* decl (odbc::decl (i->decl));

    // Get this declaration's namespace and unwind our scope until
    // we find a common prefix of namespaces.
    //
    vector<cl::NamespaceDecl const*> chain;
    ns_chain (decl, chain);

    string ns;
    for (cl::NamespaceDecl const* n: chain)
      ns += "::" + n->getName ().str ();

    string pfx;

    for (pfx = scope_->fq_name (); !pfx.empty (); pfx = scope_->fq_name ())
    {
      size_t n (pfx.size ());

      // Make sure we handle cases like ns="::foobar", pfx="::foo".
      //
      if (ns.compare (0, n, pfx) == 0 && (ns.size () == n || ns[n] == ':'))
        break;

      if (trace)
        ts << "closing namespace " << scope_->name () << endl;

      scope_ = &scope_->scope_ ();
    }

    // Build the rest of the namespace hierarchy for this declaration.
    //
    if (ns != pfx)
    {
      location_t dl (dloc (decl));
      path f (LOCATION_FILE (dl));
      size_t l (LOCATION_LINE (dl));
      size_t c (LOCATION_COLUMN (dl));

      // Number of namespaces already open.
      //
      size_t k (0);
      for (size_t p (pfx.find ("::")); p != string::npos;
           p = pfx.find ("::", p + 2))
        ++k;

      for (; k < chain.size (); ++k)
      {
        string n (chain[k]->getName ().str ());

        if (trace)
          ts << "opening namespace " << n << " for " << f << ":" << l
             << endl;

        // Use the declarations's file, line, and column as an
        // approximation for this namespace origin.
        //
        tree tree_node (odbc::make (chain[k]));

        namespace_& node (unit_->new_node<namespace_> (f, l, c, tree_node));
        unit_->new_edge<defines> (*scope_, node, n);

        if (namespace_* orig =
            dynamic_cast<namespace_*> (unit_->find (tree_node)))
        {
          // This is an extension.
          //
          node.original (*orig);
        }
        else
        {
          // This is the original. Add it to the map and process any
          // pragmas it might have (at this stage namespaces can only
          // have name pragmas).
          //
          unit_->insert (tree_node, node);
          process_named_pragmas (tree_node, node);
        }

        scope_ = &node;
      }
    }

    if (cl::ClassTemplateDecl const* t =
          cl::dyn_cast<cl::ClassTemplateDecl> (decl))
      emit_template_decl (t);
    else
    {
      type* n (emit_type_decl (decl));

      // If this is a named class-type definition, then handle
      // the pragmas.
      //
      if (n != 0)
        process_pragmas (n->tree_node (), *n, n->name (), b, i, e);
    }

    assert (class_scopes_.empty ());
  }

  // Diagnose any position pragmas that haven't been associated.
  //
  diagnose_unassoc_pragmas (decls_);
}

type* parser::impl::
emit_type_decl (cl::Decl const* decl)
{
  if (cl::TagDecl const* td = cl::dyn_cast<cl::TagDecl> (decl))
  {
    string name (td->getName ().str ());
    cl::Decl const* ld (td); // Declaration for location.

    if (name.empty ())
    {
      // If we have an anonymous class typedef, use the user-supplied
      // name instead. ARM says that in typedef struct {} S; S becomes
      // struct's name.
      //
      if (cl::TypedefNameDecl const* tn = td->getTypedefNameForAnonDecl ())
      {
        name = tn->getName ().str ();
        ld = tn;
      }
      else
      {
        // This type has no name which means that it is either typedef'ed
        // as a derived type or it is used to declare a varibale or
        // similar. The first case will be covered by the typedef handling
        // code below. The second case will be covere by emit_type().
        //
        return 0;
      }
    }

    location_t dl (dloc (ld));
    path file (LOCATION_FILE (dl));
    size_t line (LOCATION_LINE (dl));
    size_t clmn (LOCATION_COLUMN (dl));

    tree t (tag_type (td));
    gcc_tree_code_type tc (TREE_CODE (t));

    if (trace)
      ts << "start " << gcc_tree_code_name (tc) << " " << name
         << " at " << file << ":" << line << endl;

    type* node (0);

    switch (tc)
    {
    case RECORD_TYPE:
      {
        node = &emit_class<class_> (t, file, line, clmn);
        break;
      }
    case UNION_TYPE:
      {
        node = &emit_union<union_> (t, file, line, clmn);
        break;
      }
    case ENUMERAL_TYPE:
      {
        node = &emit_enum (t, decl_access (td), file, line, clmn);
        break;
      }
    default:
      return 0;
    }

    if (trace)
      ts << "end " << gcc_tree_code_name (tc) << " " << name
         << " (" << node << ") at " << file << ":" << line << endl;

    if (COMPLETE_TYPE_P (t))
      unit_->new_edge<defines> (*scope_, *node, name);
    else
      unit_->new_edge<declares> (*scope_, *node, name);

    return node;
  }

  // Normal typedef.
  //
  cl::TypedefNameDecl const* td (cl::cast<cl::TypedefNameDecl> (decl));
  tree dt (odbc::make (td));
  tree t (TREE_TYPE (dt));
  string name (td->getName ().str ());

  // We need to detect and ignore the anonymous class typedef case
  // described above since we already used this name to define the
  // class.
  //
  {
    cl::QualType ut (td->getUnderlyingType ());

    if (cl::TagDecl const* tag = ut.getCanonicalType ()->getAsTagDecl ())
    {
      if (tag->getTypedefNameForAnonDecl () == td)
        return 0;
    }
  }

  location_t dl (dloc (td));
  path f (LOCATION_FILE (dl));
  size_t l (LOCATION_LINE (dl));
  size_t c (LOCATION_COLUMN (dl));

  type& node (emit_type (t, decl_access (td), f, l, c));

  // Omit inner self-typedefs (e.g., a class typedefs itself in its own
  // scope). Such aliases don't buy us anything (in particular, they cannot
  // be used to form an fq-name) but they do cause scoping cycles if this
  // name happens to be used to find outer scope (see scope::scope_()).
  // Note that this means we can now have class template instantiations that
  // are not named and therefore don't belong to any scope.
  //
  // Note that emit_type() might still enter this decl as a hint. It's fuzzy
  // whether this is harmless or not.
  //
  // Note also that using the normal scope hierarchy does not work in more
  // complex cases where templates cross-self-typedef. So instead we use
  // a special-purpose mechanism (class_scopes_). Note for this to work
  // correctly (hopefully), the class should be "in scope" for its bases.
  // Here is a representative examples (inspired by code in Eigen):
  //
  // template <typename M>
  // struct PlainObjectBase
  // {
  //   typedef M Self;
  // };
  //
  // template <typename T, int X, int Y>
  // struct Matrix: PlainObjectBase<Matrix<T, X, Y>>
  // {
  //   typedef PlainObjectBase<Matrix> Base;
  //   typedef Matrix Self;
  // };
  //
  // typedef Matrix<double, 3, 1> Vector3d;
  //
  // Here we want both Self's (but not Base) to be skipped.
  //
  if (scope* s = dynamic_cast<scope*> (&node))
  {
    for (auto i (class_scopes_.rbegin ()); i != class_scopes_.rend (); ++i)
    {
      if (s == *i)
      {
        if (trace)
          ts << "omitting inner self-typedef " << emit_type_name (t)
             << " (" << &node << ") -> " << name << " at " << f << ":" << l
             << endl;
        return 0;
      }
    }
  }

  typedefs& edge (unit_->new_edge<typedefs> (*scope_, node, name));

  // Find our hint.
  //
  if (tree ot = DECL_ORIGINAL_TYPE (dt))
  {
    if (names* hint = unit_->find_hint (ot))
      edge.hint (*hint);
  }

  // Add this edge to the hint map. It may already be there if we
  // are handling something like this:
  //
  // typedef foo bar;
  // typedef bar foo;
  //
  // So use the latest seen name.
  //
  unit_->insert_hint (t, edge);

  if (trace)
    ts << "typedef " << emit_type_name (t) << " (" << &node << ") -> "
       << name << " at " << f << ":" << l << endl;

  return 0;
}

void parser::impl::
emit_template_decl (cl::ClassTemplateDecl const* decl)
{
  // Currently we only handle class/union templates.
  //
  cl::CXXRecordDecl const* r (decl->getTemplatedDecl ());
  string name (decl->getName ().str ());

  location_t dl (dloc (decl));

  if (trace)
    ts << "start " << (r->isUnion () ? "union" : "class") << " template "
       << name << " at " << LOCATION_FILE (dl) << ":" << LOCATION_LINE (dl)
       << endl;

  type_template* t_node (0);

  if (r->isUnion ())
    t_node = &emit_union_template (decl);
  else
    t_node = &emit_class_template (decl);

  if (r->getDefinition () != 0)
    unit_->new_edge<defines> (*scope_, *t_node, name);
  else
    unit_->new_edge<declares> (*scope_, *t_node, name);

  if (trace)
    ts << "end " << (r->isUnion () ? "union" : "class") << " template "
       << name << " (" << t_node << ") at " << LOCATION_FILE (dl) << ":"
       << LOCATION_LINE (dl) << endl;
}

class_template& parser::impl::
emit_class_template (cl::ClassTemplateDecl const* t, bool stub)
{
  // See if there is a stub already for this template.
  //
  class_template* ct_node (0);
  tree k (template_key (t));

  if (node* n = unit_->find (k))
  {
    ct_node = &dynamic_cast<class_template&> (*n);
  }
  else
  {
    location_t dl (dloc (t));
    path f (LOCATION_FILE (dl));
    size_t ln (LOCATION_LINE (dl));
    size_t cl (LOCATION_COLUMN (dl));

    ct_node = &unit_->new_node<class_template> (f, ln, cl, k);
    unit_->insert (k, *ct_node);
  }

  cl::CXXRecordDecl const* def (t->getTemplatedDecl ()->getDefinition ());

  if (stub || def == 0)
    return *ct_node;

  class_scopes_.push_back (ct_node);

  // Collect member declarations so that we can traverse them in
  // the source code order. For now we are only interested in
  // nested class template declarations.
  //
  decl_set decls;
  collect_members (def, decls, true);

  scope* prev_scope (scope_);
  scope_ = ct_node;

  for (decl_set::const_iterator i (decls.begin ()), e (decls.end ());
       i != e; ++i)
  {
    if (i->prag)
      continue;

    if (cl::ClassTemplateDecl const* d =
          cl::dyn_cast<cl::ClassTemplateDecl> (odbc::decl (i->decl)))
      emit_template_decl (d);
  }

  // Diagnose any position pragmas that haven't been associated.
  //
  diagnose_unassoc_pragmas (decls);

  scope_ = prev_scope;
  class_scopes_.pop_back ();
  return *ct_node;
}

union_template& parser::impl::
emit_union_template (cl::ClassTemplateDecl const* t, bool stub)
{
  // See if there is a stub already for this template.
  //
  union_template* ut_node (0);
  tree k (template_key (t));

  if (node* n = unit_->find (k))
  {
    ut_node = &dynamic_cast<union_template&> (*n);
  }
  else
  {
    location_t dl (dloc (t));
    path f (LOCATION_FILE (dl));
    size_t l (LOCATION_LINE (dl));
    size_t c (LOCATION_COLUMN (dl));

    ut_node = &unit_->new_node<union_template> (f, l, c, k);
    unit_->insert (k, *ut_node);
  }

  cl::CXXRecordDecl const* def (t->getTemplatedDecl ()->getDefinition ());

  if (stub || def == 0)
    return *ut_node;

  class_scopes_.push_back (ut_node);

  decl_set decls;
  collect_members (def, decls, true);

  scope* prev_scope (scope_);
  scope_ = ut_node;

  for (decl_set::const_iterator i (decls.begin ()), e (decls.end ());
       i != e; ++i)
  {
    if (i->prag)
      continue;

    if (cl::ClassTemplateDecl const* d =
          cl::dyn_cast<cl::ClassTemplateDecl> (odbc::decl (i->decl)))
      emit_template_decl (d);
  }

  // Diagnose any position pragmas that haven't been associated.
  //
  diagnose_unassoc_pragmas (decls);

  scope_ = prev_scope;
  class_scopes_.pop_back ();
  return *ut_node;
}

enum_& parser::impl::
emit_enum (tree e,
           access access,
           path const& file,
           size_t line,
           size_t clmn,
           bool stub)
{
  e = TYPE_MAIN_VARIANT (e);

  cl::EnumDecl const* ed (
    cl::cast<cl::EnumDecl> (odbc::type (e)->getAsTagDecl ()));

  // See if there is a stub already for this type.
  //
  enum_* e_node (0);

  if (node* n = unit_->find (e))
    e_node = &dynamic_cast<enum_&> (*n);
  else
  {
    e_node = &unit_->new_node<enum_> (file, line, clmn, e);

    // Set the underlying type even for incomplete (forward-declared) enums.
    //
    cl::QualType ut (ed->getIntegerType ());

    if (ut.isNull ())
      ut = odbc::ast ().IntTy;

    names* hint (unit_->find_hint (odbc::make (ut)));
    integral_type* un = dynamic_cast<integral_type*> (
      unit_->find (odbc::make (main_variant (ut))));

    if (un != 0)
    {
      underlies& edge (unit_->new_edge<underlies> (*un, *e_node));

      if (hint != 0)
        edge.hint (*hint);
    }

    unit_->insert (e, *e_node);
  }

  cl::EnumDecl const* def (ed->getDefinition ());

  if (stub || !COMPLETE_TYPE_P (e) || def == 0)
    return *e_node;

  // Traverse enumerators.
  //
  for (cl::EnumConstantDecl const* ec: def->enumerators ())
  {
    string name (ec->getName ().str ());
    tree decl (odbc::make (ec));

    llvm::APSInt const& v (ec->getInitVal ());
    unsigned long long val (
      v.isSigned ()
      ? static_cast<unsigned long long> (v.getExtValue ())
      : v.getZExtValue ());

    location_t dl (dloc (ec));
    path f (LOCATION_FILE (dl));
    size_t l (LOCATION_LINE (dl));
    size_t c (LOCATION_COLUMN (dl));

    enumerator& er_node = unit_->new_node<enumerator> (f, l, c, decl, val);
    unit_->new_edge<enumerates> (*e_node, er_node);
    unit_->insert (decl, er_node);

    // In C++11 the enumerators are always available in the enum's
    // scope, even for old enums.
    //
    if (ops_.std () >= cxx_version::cxx11)
      unit_->new_edge<names> (*e_node, er_node, name, access::public_);

    // Inject enumerators into the outer scope unless this is an
    // enum class.
    //
    if (!def->isScoped ())
      unit_->new_edge<names> (*scope_, er_node, name, access);

    if (trace)
      ts << "\tenumerator " << name << " at " << f << ":" << l << endl;
  }

  return *e_node;
}

void parser::impl::
emit_member (cl::FieldDecl const* d,
             scope& s,
             char const* kind,
             decl_set::const_iterator b,
             decl_set::const_iterator i,
             decl_set::const_iterator e)
{
  // Bit-fields can be anonymous, which we ignore. Another case where we
  // can have no name is anonymous struct or union extension, for
  // example:
  //
  // struct s
  // {
  //   union
  //   {
  //     int a;
  //     int b;
  //   };
  // };
  //
  // Ignore such members for now.
  //
  if (d->getName ().empty ())
    return;

  tree dt (odbc::make (d));
  tree t (odbc::make (d->getType ()));
  string name (d->getName ().str ());

  location_t dl (dloc (d));
  path file (LOCATION_FILE (dl));
  size_t line (LOCATION_LINE (dl));
  size_t clmn (LOCATION_COLUMN (dl));

  access a (decl_access (d));

  type& type_node (emit_type (t, a, file, line, clmn));
  data_member& member_node (
    unit_->new_node<data_member> (file, line, clmn, dt));
  unit_->insert (dt, member_node);

  unit_->new_edge<names> (s, member_node, name, a);
  belongs& edge (unit_->new_edge<belongs> (member_node, type_node));

  // See if there is a name hint for this type.
  //
  if (names* hint = unit_->find_hint (t))
    edge.hint (*hint);

  if (trace)
    ts << "\t" << a.string () << " " << kind << " " << emit_type_name (t)
       << " (" << &type_node << ") " << name << " at "
       << file << ":" << line << endl;

  // Process pragmas that may be associated with this field.
  //
  process_pragmas (dt, member_node, name, b, i, e);
}

type& parser::impl::
emit_type (tree t,
           access access,
           path const& file,
           size_t line,
           size_t clmn)
{
  if (t.kind () == tree::type_decl_kind)
    t = TREE_TYPE (t);

  cl::QualType qt (odbc::type (t));
  cl::Qualifiers quals;
  tree mv (odbc::make (main_variant (qt, quals)));

  node* n (unit_->find (mv));

  type& r (n != 0
           ? dynamic_cast<type&> (*n)
           : create_type (t, access, file, line, clmn));

  if (trace && n != 0)
    ts << "found node " << &r << " for type " << emit_type_name (mv) << endl;

  bool qc (quals.hasConst ());
  bool qv (quals.hasVolatile ());
  bool qr (quals.hasRestrict ());

  if (!qc && !qv && !qr)
  {
    unit_->insert (t, r); // Add this variant to the map.
    return r;
  }

  // See if this type already has this variant.
  //
  for (type::qualified_iterator i (r.qualified_begin ());
       i != r.qualified_end (); ++i)
  {
    qualifier& q (i->qualifier ());

    if (q.const_ () == qc && q.volatile_ () == qv && q.restrict_ () == qr)
    {
      if (trace)
        ts << "found qualifier variant " << &q << endl;

      unit_->insert (t, q); // Add this variant to the map.
      return q;
    }
  }

  // No such variant yet. Create a new one.
  //
  qualifier& q (unit_->new_node<qualifier> (file, line, clmn, t, qc, qv, qr));
  qualifies& e (unit_->new_edge<qualifies> (q, r));
  unit_->insert (t, q);

  // See if there is a name hint for the base type. We have two possible
  // cases: an inline qualifier, as in:
  //
  // const foo x;
  //
  // And a qualifier typedef:
  //
  // typedef const foo cfoo;
  // cfoo x;
  //
  // In the latter case we desugar the typedef until we get to the
  // qualified type.
  //
  {
    cl::QualType s (qt);

    while (!s.hasLocalQualifiers ())
    {
      cl::QualType n (s.getSingleStepDesugaredType (odbc::ast ()));

      if (n == s)
        break;

      s = n;
    }

    if (s.hasLocalQualifiers ())
    {
      if (names* hint = unit_->find_hint (
            odbc::make (s.getLocalUnqualifiedType ())))
        e.hint (*hint);
    }
  }

  process_named_pragmas (TYPE_MAIN_VARIANT (t), q);

  return q;
}

type& parser::impl::
create_type (tree t,
             access access,
             path const& file,
             size_t line,
             size_t clmn)
{
  type* r (0);

  cl::ASTContext& ctx (odbc::ast ());
  cl::QualType qt (odbc::type (t));
  cl::QualType mvt (main_variant (qt));
  tree mv (odbc::make (mvt));
  gcc_tree_code_type tc (TREE_CODE (mv));

  switch (tc)
  {
    //
    // User-defined types.
    //
  case RECORD_TYPE:
  case UNION_TYPE:
    {
      t = mv;
      cl::CXXRecordDecl const* rd (odbc::record_decl (t));

      cl::ClassTemplateSpecializationDecl const* sd (
        rd != 0
        ? cl::dyn_cast<cl::ClassTemplateSpecializationDecl> (rd)
        : 0);

      if (sd == 0)
      {
        // Ordinary class. There are two situations which can lead
        // here. First is when we have an anonymous class that is
        // part of the declaration, for example:
        //
        // typedef const struct {...} s;
        //
        // The second situation is a named class definition which
        // we haven't parsed yet. In this case we are going to
        // create a "stub" class node which will be processed and
        // filled in later.
        //
        if (trace)
          ts << "start anon/stub " << gcc_tree_code_name (tc) << " at "
             << file << ":" << line << endl;

        if (rd == 0 || odbc::anonymous_type (t))
        {
          if (tc == RECORD_TYPE)
            r = &emit_class<class_> (t, file, line, clmn);
          else
            r = &emit_union<union_> (t, file, line, clmn);
        }
        else
        {
          // Use the "defining" declaration's file, line, and column
          // information to create the stub.
          //
          location_t dl (dloc (rd));
          path f (LOCATION_FILE (dl));
          size_t l (LOCATION_LINE (dl));
          size_t c (LOCATION_COLUMN (dl));

          if (tc == RECORD_TYPE)
            r = &emit_class<class_> (t, f, l, c, true);
          else
            r = &emit_union<union_> (t, f, l, c, true);
        }

        if (trace)
          ts << "end anon/stub " << gcc_tree_code_name (tc) << " (" << r
             << ")" << " at " << file << ":" << line << endl;
      }
      else
      {
        // Template instantiation.
        //
        cl::ClassTemplateDecl const* decl (sd->getSpecializedTemplate ());

        // Get to the most general template declaration.
        //
        while (cl::ClassTemplateDecl const* m =
                 decl->getInstantiatedFromMemberTemplate ())
          decl = m;

        type_template* t_node (0);

        // Find the template node or create a stub if none exist.
        //
        if (node* n = unit_->find (template_key (decl)))
          t_node = &dynamic_cast<type_template&> (*n);
        else
        {
          if (tc == RECORD_TYPE)
            t_node = &emit_class_template (decl, true);
          else
            t_node = &emit_union_template (decl, true);
        }

        if (trace)
          ts << "start " << gcc_tree_code_name (tc) << " instantiation "
             << emit_type_name (t) << " for template (" << t_node << ")"
             << " at " << file << ":" << line << endl;

        type_instantiation* i_node (0);

        if (tc == RECORD_TYPE)
          i_node = &emit_class<class_instantiation> (t, file, line, clmn);
        else
          i_node = &emit_union<union_instantiation> (t, file, line, clmn);

        if (trace)
          ts << "end " << gcc_tree_code_name (tc) << " instantiation ("
             << static_cast<type*> (i_node) << ")"
             << " at " << file << ":" << line << endl;

        unit_->new_edge<instantiates> (*i_node, *t_node);
        process_named_pragmas (t, *i_node);
        r = i_node;
      }

      break;
    }
  case ENUMERAL_TYPE:
    {
      // The same logic as in the "ordinary class" case above
      // applies here.
      //
      t = mv;
      cl::TagDecl const* ed (mvt->getAsTagDecl ());

      if (odbc::anonymous_type (t))
        r = &emit_enum (t, access, file, line, clmn);
      else
      {
        location_t dl (dloc (ed->getDefinition () != 0
                             ? ed->getDefinition ()
                             : ed));
        path f (LOCATION_FILE (dl));
        size_t l (LOCATION_LINE (dl));
        size_t c (LOCATION_COLUMN (dl));

        r = &emit_enum (t, access, f, l, c, true);
      }

      break;
    }

    //
    // Derived types.
    //

  case ARRAY_TYPE:
    {
      cl::ArrayType const* at (ctx.getAsArrayType (qt));
      unsigned long long size (0);

      if (cl::ConstantArrayType const* ca =
            cl::dyn_cast<cl::ConstantArrayType> (at))
        size = ca->getZExtSize ();
      else if (!cl::isa<cl::IncompleteArrayType> (at))
      {
        error (file, line, clmn)
          << "non-integer array index" << endl;

        throw failed ();
      }

      // In our semantic graph model we have a separate type node for
      // qualifiers. So we strip the qualification from the element type
      // and only preserve it in the array type. In other words, we view
      // it as "constant array" rather than "array of constant elements".
      //
      using semantics::array; // vs std::array.

      cl::QualType bt (at->getElementType ());
      tree bt_mv (odbc::make (main_variant (bt)));
      type& bt_node (emit_type (bt_mv, access::public_, file, line, clmn));
      t = mv;
      array& a (unit_->new_node<array> (file, line, clmn, t, size));
      unit_->insert (t, a);
      contains& edge (unit_->new_edge<contains> (a, bt_node));

      // See if there is a name hint for the base type.
      //
      if (names* hint = unit_->find_hint (
            bt.hasLocalQualifiers () ? bt_mv : odbc::make (bt)))
        edge.hint (*hint);

      process_named_pragmas (t, a);
      r = &a;
      break;
    }
  case REFERENCE_TYPE:
    {
      tree bt (odbc::make (qt->getAs<cl::ReferenceType> ()->getPointeeType ()));
      type& bt_node (emit_type (bt, access::public_, file, line, clmn));
      t = mv;
      reference& ref (unit_->new_node<reference> (file, line, clmn, t));
      unit_->insert (t, ref);
      references& edge (unit_->new_edge<references> (ref, bt_node));

      // See if there is a name hint for the base type.
      //
      if (names* hint = unit_->find_hint (bt))
        edge.hint (*hint);

      process_named_pragmas (t, ref);
      r = &ref;
      break;
    }
  case POINTER_TYPE:
    {
      tree bt (odbc::make (qt->getPointeeType ()));
      type& bt_node (emit_type (bt, access::public_, file, line, clmn));
      t = mv;
      pointer& p (unit_->new_node<pointer> (file, line, clmn, t));
      unit_->insert (t, p);
      points& edge (unit_->new_edge<points> (p, bt_node));

      // See if there is a name hint for the base type.
      //
      if (names* hint = unit_->find_hint (bt))
        edge.hint (*hint);

      process_named_pragmas (t, p);
      r = &p;
      break;
    }
  case OFFSET_TYPE:
    {
      t = mv;
      char const* k (
        mvt->getAs<cl::MemberPointerType> ()->isMemberFunctionPointer ()
        ? "pointer_to_member_function_type"
        : "pointer_to_data_member_type");

      r = &unit_->new_node<unsupported_type> (file, line, clmn, t, k);
      unit_->insert (t, *r);

      if (trace)
        ts << "unsupported " << k << " (" << r << ")"
           << " at " << file << ":" << line << endl;
      break;
    }
  default:
    {
      t = mv;
      r = &unit_->new_node<unsupported_type> (
        file, line, clmn, t, gcc_tree_code_name (tc));
      unit_->insert (t, *r);

      if (trace)
        ts << "unsupported " << gcc_tree_code_name (tc) << " (" << r << ")"
           << " at " << file << ":" << line << endl;

      break;
    }
  }

  return *r;
}

string parser::impl::
emit_type_name (tree t)
{
  cl::QualType qt (any_type (t));
  return qt.isNull () ? string ("<unknown>") : odbc::type_as_string (qt);
}

void parser::impl::
process_pragmas (declaration const& decl,
                 node& node,
                 string const& name,
                 decl_set::const_iterator begin,
                 decl_set::const_iterator cur,
                 decl_set::const_iterator /*end*/)
{
  // First process the position pragmas by iterating backwards until
  // we get to the preceding non-pragma declaration that has been
  // associated.
  //
  pragma_set prags;

  if (cur != begin)
  {
    decl_set::const_iterator i (cur);
    for (--i; i != begin && (i->prag != 0 || !i->assoc); --i) ;

    for (; i != cur; ++i)
    {
      if (i->prag == 0) // Skip declarations.
        continue;

      assert (!i->assoc);

      if (i->prag->check (decl, name, i->prag->pragma_name, i->prag->loc))
        prags.insert (*i->prag);
      else
        error_++; // Diagnostic has already been issued.

      i->assoc = true; // Mark this pragma as associated.
    }

    cur->assoc = true; // Mark the declaration as associated.
  }

  // Now see if there are any named pragmas for this declaration. By
  // doing this after handling the position pragmas we ensure correct
  // overriding.
  //
  {
    decl_pragmas::const_iterator i (decl_pragmas_.find (decl));

    if (i != decl_pragmas_.end ())
      prags.insert (i->second.begin (), i->second.end ());
  }

  // Finally, copy the resulting pragma set to context.
  //
  for (pragma_set::iterator i (prags.begin ()); i != prags.end (); ++i)
    add_pragma (node, i->second);
}

void parser::impl::
process_named_pragmas (declaration const& decl, node& node)
{
  pragma_set prags;

  decl_pragmas::const_iterator i (decl_pragmas_.find (decl));

  if (i != decl_pragmas_.end ())
    prags.insert (i->second.begin (), i->second.end ());

  // Copy the resulting pragma set to context.
  //
  for (pragma_set::iterator i (prags.begin ()); i != prags.end (); ++i)
    add_pragma (node, i->second);
}

void parser::impl::
add_pragma (node& n, pragma const& p)
{
  if (trace)
    ts << "\t\t pragma " << p.pragma_name << endl;

  string const& k (p.context_name);

  if (p.add == 0)
  {
    n.set (k, p.value);
    n.set (k + "-location", p.loc);
  }
  else
    p.add (n, k, p.value, p.loc);
}

void parser::impl::
diagnose_unassoc_pragmas (decl_set const& decls)
{
  for (decl_set::const_iterator i (decls.begin ()), e (decls.end ());
       i != e; ++i)
  {
    if (i->prag && !i->assoc)
    {
      pragma const& p (*i->prag);
      error (p.loc)
        << "db pragma '" << p.pragma_name << "' is not associated with a "
        << "declaration" << endl;
      error_++;
    }
  }
}

void parser::impl::
ns_chain (cl::Decl const* d, vector<cl::NamespaceDecl const*>& r)
{
  for (cl::DeclContext const* dc (d->getDeclContext ());
       dc != 0;
       dc = dc->getParent ())
  {
    if (cl::NamespaceDecl const* n = cl::dyn_cast<cl::NamespaceDecl> (dc))
    {
      // If this is an inline namespace, pretend it doesn't exist.
      //
      if (!n->isInline ())
        r.insert (r.begin (), n->getCanonicalDecl ());
    }
  }
}

//
// parser
//

parser::
~parser ()
{
  // Needs parser::impl definition.
}

parser::
parser (options const& ops,
        loc_pragmas& lp,
        ns_loc_pragmas& nslp,
        decl_pragmas& dp)
    : impl_ (new impl (ops, lp, nslp, dp))
{
}

unique_ptr<unit> parser::
parse (tree global_scope, path const& main_file)
{
  return impl_->parse (global_scope, main_file);
}

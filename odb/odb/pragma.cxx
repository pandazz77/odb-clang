// file      : odb/pragma.cxx
// license   : GNU GPL v3; see accompanying LICENSE file

#include <odb/clang.hxx>

#include <cctype> // std::isalnum
#include <limits>
#include <vector>
#include <sstream>

#include <odb/diagnostics.hxx>
#include <odb/lookup.hxx>
#include <odb/pragma.hxx>
#include <odb/cxx-token.hxx>
#include <odb/cxx-lexer.hxx>

#include <odb/context.hxx>
#include <odb/relational/context.hxx>

#include <clang/Basic/TargetInfo.h>
#include <clang/Lex/LiteralSupport.h>
#include <clang/Lex/Pragma.h>
#include <clang/Lex/Preprocessor.h>

using namespace std;
using namespace cutl;

using container::any;

// The scope of the pragma currently being processed (see
// process_odb_pragmas() below).
//
static tree current_scope_;

static inline tree
current_scope ()
{
  return current_scope_;
}

virt_declarations virt_declarations_;
loc_pragmas loc_pragmas_;
decl_pragmas decl_pragmas_;
ns_loc_pragmas ns_loc_pragmas_;

database pragma_db_;
multi_database pragma_multi_;

template <typename X>
void
accumulate (compiler::context& ctx, string const& k, any const& v, location_t)
{
  // Empty values are used to indicate that this pragma must be ignored.
  //
  if (v.empty ())
    return;

  typedef vector<X> container;

  container& c (ctx.count (k)
                ? ctx.get<container> (k)
                : ctx.set (k, container ()));

  c.push_back (v.value<X> ());
}

// Parse a qualified string. It can be in one of the following formats:
//
// "foo.bar.baz"     (can have empty leading component, e.g., ".bar.baz")
// "foo"."bar"."baz" (can have empty leading component, e.g., ""."bar"."baz")
// ."foo.bar"        (used to specify unqualifed names with periods)
//
// Empty leading components means fully qualified (similar to ::foo in C++).
//
static bool
parse_qname (cxx_lexer& l,
             cpp_ttype& tt,
             string& tl,
             tree& tn,
             string const& p, // Pragma name for diagnostic.
             qname& name,
             bool* expr = 0,       // If specified, detect an expression
             string* expr_str = 0) // and store it here.
{
  assert (tt == CPP_STRING || tt == CPP_DOT);

  // Handle the special case of unqualified name which can contain periods.
  //
  if (tt == CPP_DOT)
  {
    tt = l.next (tl, &tn);

    if (tt != CPP_STRING)
    {
      error (l) << "name expected after '.' in db pragma " << p << endl;
      return false;
    }

    name = tl;
    tt = l.next (tl, &tn);
    return true;
  }

  name.clear ();
  string str (tl);

  // See what comes after the string.
  //
  tt = l.next (tl, &tn);

  if (tt == CPP_DOT)
  {
    name.append (str);

    for (; tt == CPP_DOT; tt = l.next (tl, &tn))
    {
      tt = l.next (tl, &tn);

      if (tt != CPP_STRING)
      {
        error (l) << "name expected after '.' in db pragma " << p << endl;
        return false;
      }

      name.append (tl);
    }

    return true;
  }

  if (expr != 0 && tt == CPP_PLUS)
  {
    *expr = true;
    *expr_str = str;
    return true;
  }

  // Scan the string looking for '.' as well as for non-identifier
  // characters if we need to detect expressions.
  //
  string::size_type prev (string::npos);

  for (size_t i (0); i < str.size (); ++i)
  {
    char c (str[i]);

    if (c == '.')
    {
      if (prev == string::npos)
        name.append (string (str, 0, i));
      else
        name.append (string (str, prev + 1, i - prev - 1));

      prev  = i;
    }
    else if (expr != 0 && !(isalnum (c) || c == '_'))
    {
      *expr = true;
      *expr_str = str;
      return true;
    }
  }

  if (prev == string::npos)
    name.append (str);
  else
    name.append (string (str, prev + 1, string::npos));

  return true;
}

static bool
parse_expression (cxx_lexer& l,
                  cpp_ttype& tt,
                  string& tl,
                  tree& tn,
                  cxx_tokens& ts,
                  string const& prag,
                  bool balance_angles = false)
{
  // Keep reading tokens until we see a mis-matching ')' or ',' while
  // keeping track of the '()', '{}', and '<>' balance.
  //
  // @@ The below implementation of the '<>' balance tracking doesn't
  //    distinguish angle brackets from the comparison/stream operators and
  //    should be fixed one day.
  //
  size_t p_balance (0), b_balance (0), a_balance (0);

  for (; tt != CPP_EOF; tt = l.next (tl, &tn))
  {
    bool done (false);
    cxx_token ct (l.location (), tt);

    switch (tt)
    {
    case CPP_LESS:
      {
        if (balance_angles)
          a_balance++;

        break;
      }
    case CPP_GREATER:
      {
        if (balance_angles)
          a_balance--;

        break;
      }
    case CPP_LSHIFT:
      {
        if (balance_angles)
          a_balance += 2;

        break;
      }
    case CPP_RSHIFT:
      {
        if (balance_angles)
          a_balance -= 2;

        break;
      }
    case CPP_OPEN_BRACE:
      {
        b_balance++;
        break;
      }
    case CPP_CLOSE_BRACE:
      {
        b_balance--;
        break;
      }
    case CPP_OPEN_PAREN:
      {
        p_balance++;
        break;
      }
    case CPP_CLOSE_PAREN:
      {
        if (p_balance == 0 && b_balance == 0 && a_balance == 0)
          done = true;
        else
          p_balance--;
        break;
      }
    case CPP_COMMA:
      {
        if (p_balance == 0 && b_balance == 0 && a_balance == 0)
          done = true;
        else
          break;
      }
    case CPP_STRING:
      {
        ct.literal = tl;
        break;
      }
    case CPP_NAME:
  //case CPP_KEYWORD: see default:
      {
        ct.literal = tl;
        break;
      }
    case CPP_NUMBER:
      {
        switch (TREE_CODE (tn))
        {
        case INTEGER_CST:
          {
            tree_int_cst const& c (
              *static_cast<tree_int_cst const*> (tn.ptr ()));

            ostringstream os;
            os << c.value;

            switch (c.type)
            {
            case cst_long_long:          os << "LL"; break;
            case cst_unsigned_long_long: os << "ULL"; break;
            case cst_long:               os << "L"; break;
            case cst_unsigned_long:      os << "UL"; break;
            case cst_unsigned:           os << "U"; break;
            default:                     break;
            }

            ct.literal = os.str ();
            break;
          }
        case REAL_CST:
          {
            tree_real_cst const& c (
              *static_cast<tree_real_cst const*> (tn.ptr ()));

            ostringstream os;

            if (c.type == cst_float)
              os << static_cast<float> (c.value) << 'F';
            else
              os << c.value;

            ct.literal = os.str ();
            break;
          }
        default:
          {
            error (l) << "unexpected numeric constant in db pragma "
                      << prag << endl;
            return false;
          }
        }

        break;
      }
    default:
      {
        // CPP_KEYWORD is not in the cpp_ttype enumeration.
        //
        if (tt == CPP_KEYWORD)
          ct.literal = tl;

        break;
      }
    }

    if (done)
      break;

    // We don't store the tree node in ct since we converted numbers to
    // string literals.
    //
    ts.push_back (ct);
  }

  return true;
}

static string
parse_scoped_name (cxx_lexer& l,
                   cpp_ttype& tt,
                   string& tl,
                   tree& tn,
                   string const& prag)
{
  try
  {
    return lookup::parse_scoped_name (l, tt, tl, tn);
  }
  catch (lookup::invalid_name const&)
  {
    error (l) << "invalid name in db pragma " << prag << endl;
    return "";
  }
}

static tree
resolve_scoped_name (cxx_lexer& l,
                     cpp_ttype& tt,
                     string& tl,
                     tree& tn,
                     tree start_scope,
                     string& name,
                     bool is_type,
                     string const& prag,
                     bool trailing_scope = false,
                     cpp_ttype* prev_tt = 0)
{
  try
  {
    cpp_ttype ptt; // Not used.
    tree r (
      lookup::resolve_scoped_name (
        l, tt, tl, tn, ptt, start_scope, name, is_type, trailing_scope));

    if (prev_tt != 0)
      *prev_tt = ptt;

    return r;
  }
  catch (lookup::invalid_name const&)
  {
    error (l) << "invalid name in db pragma " << prag << endl;
    return 0;
  }
  catch (lookup::unable_to_resolve const& e)
  {
    if (e.last ())
      error (l) << "unable to resolve " << (is_type ? "type " : "") << "name "
                << "'" << e.name () << "' in db pragma " << prag << endl;
    else
      error (l) << "unable to resolve name '" << e.name () << "' in db pragma "
                << prag << endl;

    return 0;
  }
}

// Resolve a data member in the specified scope taking into account virtual
// member declarations.
//
declaration
resolve_data_member (tree scope,
                     const cxx_tokens& name,
                     string& decl_name, // Note: appended to.
                     string const& prag)
{
  declaration decl;

  if (name.size () == 1 && name.back ().type == CPP_NAME)
  {
    virt_declarations::iterator i (virt_declarations_.find (scope));

    if (i != virt_declarations_.end ())
    {
      string const& n (name.back ().literal);

      virt_declaration_set::const_iterator j (i->second.find (n, FIELD_DECL));

      if (j != i->second.end ())
      {
        decl = declaration (*j);
        decl_name += n;
      }
    }
  }

  if (!decl)
  {
    cxx_tokens_lexer l;
    l.start (name);

    tree tn;
    string tl;
    cpp_ttype tt (l.next (tl));

    tree d (resolve_scoped_name (
              l, tt, tl, tn, scope, decl_name, false, prag));

    if (d == 0)
      return decl; // Diagnostics has already been issued.

    decl = declaration (d);
  }

  return decl;
}

static bool
check_spec_decl_type (declaration const& d,
                      string const& name,
                      string const& p,
                      location_t l)
{
  gcc_tree_code_type tc (d.tree_code ());
  bool type (TREE_CODE_CLASS (tc) == tcc_type);

  if (p == "no_id")
  {
    // No_id can be used on objects only.
    //
    if (tc != RECORD_TYPE)
    {
      error (l) << "name '" << name << "' in db pragma " << p << " does "
                << "not refer to a class" << endl;
      return false;
    }
  }
  else if (p == "id"        ||
           p == "auto"      ||
           p == "column"    ||
           p == "inverse"   ||
           p == "on_delete" ||
           p == "points_to" ||
           p == "section"   ||
           p == "load"      ||
           p == "update"    ||
           p == "version"   ||
           p == "index"     ||
           p == "unique"    ||
           p == "get"       ||
           p == "set"       ||
           p == "access")
  {
    if (tc != FIELD_DECL)
    {
      error (l) << "name '" << name << "' in db pragma " << p << " does "
                << "not refer to a data member" << endl;
      return false;
    }
  }
  else if (p == "transient")
  {
    // Transient can be used for both data members and classes (object,
    // view, or composite value).
    //
    if (tc != FIELD_DECL && tc != RECORD_TYPE)
    {
      error (l) << "name '" << name << "' in db pragma " << p << " does "
                << "not refer to a data member or class" << endl;
      return false;
    }
  }
  else if (p == "added")
  {
    // Added can be used for data members only.
    //
    if (tc != FIELD_DECL)
    {
      error (l) << "name '" << name << "' in db pragma " << p << " does "
                << "not refer to a data member" << endl;
      return false;
    }
  }
  else if (p == "deleted")
  {
    // Deleted can be used for both data members and classes (object,
    // view of composite value).
    //
    if (tc != FIELD_DECL && tc != RECORD_TYPE)
    {
      error (l) << "name '" << name << "' in db pragma " << p << " does "
                << "not refer to a data member or class" << endl;
      return false;
    }
  }
  else if (p == "readonly")
  {
    // Readonly can be used for both data members and classes (object or
    // composite value).
    //
    if (tc != FIELD_DECL && tc != RECORD_TYPE)
    {
      error (l) << "name '" << name << "' in db pragma " << p << " does "
                << "not refer to a data member or class" << endl;
      return false;
    }
  }
  else if (p == "abstract" ||
           p == "callback" ||
           p == "query" ||
           p == "object" ||
           p == "optimistic" ||
           p == "polymorphic" ||
           p == "definition" ||
           p == "sectionable" ||
           p == "bulk")
  {
    if (tc != RECORD_TYPE)
    {
      error (l) << "name '" << name << "' in db pragma " << p << " does "
                << "not refer to a class" << endl;
      return false;
    }
  }
  else if (p == "pointer")
  {
    // Table can be used for namespaces and classes (object or view).
    //
    if (tc != NAMESPACE_DECL && tc != RECORD_TYPE)
    {
      error (l) << "name '" << name << "' in db pragma " << p << " does "
                << "not refer to a class" << endl;
      return false;
    }
  }
  else if (p == "table")
  {
    // Table can be used for namespaces, members (container), and types
    // (container, object, or view).
    //
    if (tc != NAMESPACE_DECL && tc != FIELD_DECL && !type)
    {
      error (l) << "name '" << name << "' in db pragma " << p << " does "
                << "not refer to a namespace, type, or data member" << endl;
      return false;
    }
  }
  else if (p == "session")
  {
    // Session can be used only for namespaces and objects.
    //
    if (tc != NAMESPACE_DECL && tc != RECORD_TYPE)
    {
      error (l) << "name '" << name << "' in db pragma " << p << " does "
                << "not refer to a namespace or class" << endl;
      return false;
    }
  }
  else if (p == "schema")
  {
    // For now schema can be used only for namespaces and objects.
    //
    if (tc != NAMESPACE_DECL && tc != RECORD_TYPE)
    {
      error (l) << "name '" << name << "' in db pragma " << p << " does "
                << "not refer to a namespace or class" << endl;
      return false;
    }
  }
  else if (p == "type" ||
           p == "id_type" ||
           p == "value_type" ||
           p == "index_type" ||
           p == "key_type")
  {
    // Type can be used for both members and types.
    //
    if (tc != FIELD_DECL && !type)
    {
      error (l) << "name '" << name << "' in db pragma " << p << " does "
                << "not refer to a type or data member" << endl;
      return false;
    }
  }
  else if (p == "default")
  {
    // Default can be used for both members and types.
    //
    if (tc != FIELD_DECL && !type)
    {
      error (l) << "name '" << name << "' in db pragma " << p << " does "
                << "not refer to a type or data member" << endl;
      return false;
    }
  }
  else if (p == "value_column" ||
           p == "index_column" ||
           p == "key_column" ||
           p == "id_column")
  {
    // Container columns can be used for both members (container) and
    // types (container).
    //
    if (tc != FIELD_DECL && !type)
    {
      error (l) << "name '" << name << "' in db pragma " << p << " does "
                << "not refer to a type or data member" << endl;
      return false;
    }
  }
  else if (p == "options" ||
           p == "value_options" ||
           p == "index_options" ||
           p == "key_options" ||
           p == "id_options")
  {
    // Options can be used for both members and types.
    //
    if (tc != FIELD_DECL && !type)
    {
      error (l) << "name '" << name << "' in db pragma " << p << " does "
                << "not refer to a type or data member" << endl;
      return false;
    }
  }
  else if (p == "null" ||
           p == "not_null" ||
           p == "key_null" ||
           p == "key_not_null" ||
           p == "value_null" ||
           p == "value_not_null")
  {
    // Null pragmas can be used for both members and types (values,
    // containers, and pointers).
    //
    if (tc != FIELD_DECL && !type)
    {
      error (l) << "name '" << name << "' in db pragma " << p << " does "
                << "not refer to a type or data member" << endl;
      return false;
    }
  }
  else if (p == "unordered")
  {
    // Unordered can be used for both members (container) and
    // types (container).
    //
    if (tc != FIELD_DECL && !type)
    {
      error (l) << "name '" << name << "' in db pragma " << p << " does "
                << "not refer to a type or data member" << endl;
      return false;
    }
  }
  else if (p == "direct_load" || p == "indirect_load")
  {
    // Direct/indirect can only be specified for data members (for now).
    //
    if (tc != FIELD_DECL)
    {
      error (l) << "name '" << name << "' in db pragma " << p << " does "
                << "not refer to a data member" << endl;
      return false;
    }
  }
  else if (p == "virtual")
  {
    // Virtual is specified for a member.
    //
    if (tc != FIELD_DECL)
    {
      error (l) << "name '" << name << "' in db pragma " << p << " does "
                << "not refer to a data member" << endl;
      return false;
    }
  }
  else if (p == "simple" ||
           p == "container")
  {
    // Apply to both members and types.
    //
    if (tc != FIELD_DECL && !type)
    {
      error (l) << "name '" << name << "' in db pragma " << p << " does "
                << "not refer to a type or data member" << endl;
      return false;
    }
  }
  else
  {
    error (l) << "unknown db pragma " << p << endl;
    return false;
  }

  return true;
}

static void
add_pragma (pragma const& prag, declaration const& decl, bool ns)
{
  if (decl)
    decl_pragmas_[decl].insert (prag);
  else
  {
    tree scope (current_scope ());

    if (!ns)
    {
      if (!CLASS_TYPE_P (scope))
        scope = global_namespace;

      loc_pragmas_[scope].push_back (prag);
    }
    else
      ns_loc_pragmas_.push_back (ns_loc_pragma (scope, prag));
  }
}

static void
handle_pragma (cxx_lexer& l,
               string db,
               string p,
               string const& qualifier,
               any& qualifier_value,
               declaration const& decl,
               string const& decl_name,
               bool ns) // True if this is a position namespace pragma.
{
  cpp_ttype tt;
  string tl;
  tree tn;

  // See if there is a database prefix. The db argument may already
  // contain it.
  //
  if (db.empty () &&
      (p == "mysql"  ||
       p == "sqlite" ||
       p == "pgsql"  ||
       p == "oracle" ||
       p == "mssql"))
  {
    tt = l.next (tl);

    if (tt != CPP_COLON)
    {
      error (l) << "':' expected after database prefix " << p << endl;
      return;
    }

    // Specifier prefix.
    //
    db = p;
    tt = l.next (p);

    if (tt != CPP_NAME && tt != CPP_KEYWORD)
    {
      error (l) << "expected specifier after databas prefix " << db << endl;
      return;
    }
  }

  string name (p);                           // Pragma name.
  any val;                                   // Pragma value.
  pragma::add_func adder (0);                // Custom context adder.
  location_t loc (l.location ());            // Pragma location.

  if (qualifier == "model")
  {
    // version(unsigned long long base,
    //         unsigned long long current,
    //         open|closed)
    //

    // Make sure we've got the correct declaration type.
    //
    assert (decl == global_namespace);

    if (p == "version")
    {
      if (l.next (tl, &tn) != CPP_OPEN_PAREN)
      {
        error (l) << "'(' expected after db pragma " << p << endl;
        return;
      }

      model_version v;

      // base
      //
      if (l.next (tl, &tn) != CPP_NUMBER || TREE_CODE (tn) != INTEGER_CST)
      {
        error (l) << "unsigned integer expected as base version" << endl;
        return;
      }

      v.base = integer_value (tn);

      if (v.base == 0)
      {
        error (l) << "base version cannot be zero" << endl;
        return;
      }

      // current
      //
      if (l.next (tl, &tn) != CPP_COMMA)
      {
        error (l) << "current version expected after base version" << endl;
        return;
      }

      if (l.next (tl, &tn) != CPP_NUMBER || TREE_CODE (tn) != INTEGER_CST)
      {
        error (l) << "unsigned integer expected as current version" << endl;
        return;
      }

      v.current = integer_value (tn);

      if (v.current == 0)
      {
        error (l) << "current version cannot be zero" << endl;
        return;
      }

      if (v.base > v.current)
      {
        error (l) << "current version should be greater than or equal to " <<
          "base version" << endl;
        return;
      }

      // open|closed
      //
      tt = l.next (tl, &tn);
      if (tt == CPP_COMMA)
      {
        if (l.next (tl, &tn) != CPP_NAME || (tl != "open" && tl != "closed"))
        {
          error (l) << "open or closed expected after current version" << endl;
          return;
        }

        v.open = (tl == "open");
        tt = l.next (tl, &tn);
      }
      else
        v.open = true;

      if (tt != CPP_CLOSE_PAREN)
      {
        error (l) << "')' expected at the end of db pragma " << p << endl;
        return;
      }

      name = "model-version";
      val = v;
      tt = l.next (tl, &tn);
    }
    else
    {
      error (l) << "unknown db pragma " << p << endl;
      return;
    }
  }
  else if (qualifier == "map")
  {
    // type("<regex>") | type(<name>)
    // as("<subst>")   | as(<name>)
    // to("<subst>")   | to(<expr>)
    // from("<subst>") | from(<expr>)
    //

    if (p != "type" &&
        p != "as"   &&
        p != "to"   &&
        p != "from")
    {
      error (l) << "unknown db pragma " << p << endl;
      return;
    }

    // Make sure we've got the correct declaration type.
    //
    assert (decl == global_namespace);

    if (l.next (tl, &tn) != CPP_OPEN_PAREN)
    {
      error (l) << "'(' expected after db pragma " << p << endl;
      return;
    }

    tt = l.next (tl, &tn);

    if (qualifier_value.type_info () == typeid (custom_cxx_type))
    {
      // C++ type mapping.
      //
      custom_cxx_type& ct (qualifier_value.value<custom_cxx_type> ());

      if (p == "type" || p == "as")
      {
        // Can be built-in type (e.g., bool).
        //
        if (tt == CPP_NAME || tt == CPP_KEYWORD || tt == CPP_SCOPE)
        {
          string name;
          tree decl (
            resolve_scoped_name (
              l, tt, tl, tn, current_scope (), name, true, p));

          if (decl == 0)
            return; // Diagnostics has already been issued.

          if (TREE_CODE (decl) != TYPE_DECL)
          {
            error (loc) << "name '" << name << "' in db pragma "
                        << p << " does not refer to a type" << endl;
            return;
          }

          (p == "type" ? ct.type_node : ct.as_node) = TREE_TYPE (decl);
          (p == "type" ? ct.type_name : ct.as_name) = name;
        }
        else
        {
          error (l) << "type name expected in db pragma " << p << endl;
          return;
        }
      }
      else if (p == "to" || p == "from")
      {
        if (tt != CPP_CLOSE_PAREN) // Empty expression is ok.
        {
          if (!parse_expression (
                l, tt, tl, tn, (p == "to" ? ct.to : ct.from), p))
            return; // Diagnostics has already been issued.
        }
      }
    }
    else
    {
      using relational::custom_db_type;

      // Database type mapping.
      //
      custom_db_type& ct (qualifier_value.value<custom_db_type> ());

      if (p == "type")
      {
        if (tt != CPP_STRING)
        {
          error (l) << "type name regex expected in db pragma " << p << endl;
          return;
        }

        try
        {
          // Make it case-insensitive.
          //
          ct.type.assign (tl, true);
        }
        catch (regex_format const& e)
        {
          error (l) << "invalid regex: '" << e.regex () << "' in db pragma "
                    << p << ": " << e.description () << endl;
          return;
        }
      }
      else if (p == "as")
      {
        if (tt != CPP_STRING)
        {
          error (l) << "type name expected in db pragma " << p << endl;
          return;
        }

        ct.as = tl;
      }
      else if (p == "to")
      {
        if (tt != CPP_STRING)
        {
          error (l) << "expression expected in db pragma " << p << endl;
          return;
        }

        ct.to = tl;
      }
      else if (p == "from")
      {
        if (tt != CPP_STRING)
        {
          error (l) << "expression expected in db pragma " << p << endl;
          return;
        }

        ct.from = tl;
      }

      tt = l.next (tl, &tn);
    }

    if (tt != CPP_CLOSE_PAREN)
    {
      error (l) << "')' expected at the end of db pragma " << p << endl;
      return;
    }

    name.clear (); // We don't need to add anything for this pragma.
    tt = l.next (tl, &tn);
  }
  else if (qualifier == "index")
  {
    // unique
    // type("<type>")
    // method("<method>")
    // options("<options>")
    // member(<name>[, "<options>"])
    // members(<name>[, <name>...])
    //

    if (p != "unique"  &&
        p != "type"    &&
        p != "method"  &&
        p != "options" &&
        p != "member"  &&
        p != "members")
    {
      error (l) << "unknown db pragma " << p << endl;
      return;
    }

    using relational::index;
    index& in (qualifier_value.value<index> ());

    if (p == "unique")
      in.type = "UNIQUE";
    else
    {
      if (l.next (tl, &tn) != CPP_OPEN_PAREN)
      {
        error (l) << "'(' expected after db pragma " << p << endl;
        return;
      }

      tt = l.next (tl, &tn);

      if (p == "type")
      {
        if (tt != CPP_STRING)
        {
          error (l) << "index type expected in db pragma " << p << endl;
          return;
        }

        in.type = tl;
        tt = l.next (tl, &tn);
      }
      else if (p == "method")
      {
        if (tt != CPP_STRING)
        {
          error (l) << "index method expected in db pragma " << p << endl;
          return;
        }

        in.method = tl;
        tt = l.next (tl, &tn);
      }
      else if (p == "options")
      {
        if (tt != CPP_STRING)
        {
          error (l) << "index options expected in db pragma " << p << endl;
          return;
        }

        in.options = tl;
        tt = l.next (tl, &tn);
      }
      else if (p == "member")
      {
        if (tt != CPP_NAME)
        {
          error (l) << "data member name expected in db pragma " << p << endl;
          return;
        }

        index::member m;
        m.loc = loc;
        m.name = tl;

        tt = l.next (tl, &tn);

        // Parse nested members if any.
        //
        for (; tt == CPP_DOT; tt = l.next (tl, &tn))
        {
          if (l.next (tl, &tn) != CPP_NAME)
          {
            error (l) << "name expected after '.' in db pragma " << p << endl;
            return;
          }

          m.name += '.';
          m.name += tl;
        }

        // Parse member options, if any.
        //
        if (tt == CPP_COMMA)
        {
          if (l.next (tl, &tn) != CPP_STRING)
          {
            error (l) << "index member options expected in db pragma " << p
                      << endl;
            return;
          }

          m.options = tl;
          tt = l.next (tl, &tn);
        }

        in.members.push_back (m);
      }
      else if (p == "members")
      {
        for (;;)
        {
          if (tt != CPP_NAME)
          {
            error (l) << "data member name expected in db pragma " << p
                      << endl;
            return;
          }

          index::member m;
          m.loc = l.location ();
          m.name = tl;

          tt = l.next (tl, &tn);

          // Parse nested members if any.
          //
          for (; tt == CPP_DOT; tt = l.next (tl, &tn))
          {
            if (l.next (tl, &tn) != CPP_NAME)
            {
              error (l) << "name expected after '.' in db pragma " << p
                        << endl;
              return;
            }

            m.name += '.';
            m.name += tl;
          }

          in.members.push_back (m);

          if (tt == CPP_COMMA)
            tt = l.next (tl, &tn);
          else
            break;
        }
      }

      if (tt != CPP_CLOSE_PAREN)
      {
        error (l) << "')' expected at the end of db pragma " << p << endl;
        return;
      }
    }

    name.clear (); // We don't need to add anything for this pragma.
    tt = l.next (tl, &tn);
  }
  else if (p == "index" ||
           p == "unique")
  {
    // index
    // unique
    //

    // Make sure we've got the correct declaration type.
    //
    if (decl && !check_spec_decl_type (decl, decl_name, p, loc))
      return;

    tt = l.next (tl, &tn);
  }
  else if (p == "get" ||
           p == "set" ||
           p == "access")
  {
    // get(name|expr)
    // set(name|expr)
    // access(name|expr)
    //

    // Make sure we've got the correct declaration type.
    //
    if (decl && !check_spec_decl_type (decl, decl_name, p, loc))
      return;

    if (l.next (tl, &tn) != CPP_OPEN_PAREN)
    {
      error (l) << "'(' expected after db pragma " << p << endl;
      return;
    }

    member_access ma (loc, p == "set" ? "modifier" : "accessor", false);

    tt = l.next (tl, &tn);
    if (tt != CPP_CLOSE_PAREN) // Empty expression are ok.
    {
      if (!parse_expression (l, tt, tl, tn, ma.expr, p, true /* balance_angles */))
        return; // Diagnostics has already been issued.

      if (tt != CPP_CLOSE_PAREN)
      {
        error (l) << "')' expected at the end of db pragma " << p << endl;
        return;
      }
    }

    val = ma;

    // Convert access to the get/set pair.
    //
    if (p == "access")
    {
      if (db.empty () || db == pragma_db_.string ())
        add_pragma (
          pragma (p, "get", val, loc, &check_spec_decl_type, 0), decl, ns);

      ma.kind = "modifier";
      val = ma;
      name = "set";
    }

    tt = l.next (tl, &tn);
  }
  else if (p == "table")
  {
    // table (<name>)
    // table (<name> [= "<alias>"] [type] [: "<cond>"]  (view only)
    //
    // <name> := "name" | "name.name" | "name"."name"
    //

    // Make sure we've got the correct declaration type.
    //
    if (decl && !check_spec_decl_type (decl, decl_name, p, loc))
      return;

    if (l.next (tl, &tn) != CPP_OPEN_PAREN)
    {
      error (l) << "'(' expected after db pragma " << p << endl;
      return;
    }

    tt = l.next (tl, &tn);

    if (tt != CPP_STRING && tt != CPP_DOT)
    {
      error (l) << "table name expected in db pragma " << p << endl;
      return;
    }

    // The table specifier is used for both objects and views. In case
    // of an object, the context values is just a string. In case of a
    // view, the context value is a view_object entry. The problem is
    // that at this stage we don't know whether we are dealing with an
    // object or a view. To resolve this in a somewhat hackish way, we
    // are going to create both a string and a view_object entry.
    //
    view_object vo;
    vo.kind = view_object::table;

    if (!parse_qname (l, tt, tl, tn, p, vo.tbl_name))
      return; // Diagnostics has already been issued.

    if (tt == CPP_EQ)
    {
      // We have an alias.
      //
      if (l.next (tl, &tn) != CPP_STRING)
      {
        error (l) << "table alias expected after '=' in db pragma " << p
                  << endl;
        return;
      }

      vo.alias = tl;
      tt = l.next (tl, &tn);
    }

    if (tt == CPP_NAME)
    {
      // We have a JOIN type.
      //
      if (tl == "left")
        vo.join = view_object::left;
      else if (tl == "right")
        vo.join = view_object::right;
      else if (tl == "full")
        vo.join = view_object::full;
      else if (tl == "inner")
        vo.join = view_object::inner;
      else if (tl == "cross")
        vo.join = view_object::cross;
      else
      {
        error (l) << "unknown join type '" << tl << "'" << endl;
        return;
      }

      tt = l.next (tl, &tn);
    }
    else
      vo.join = view_object::left;

    if (tt == CPP_COLON)
    {
      // We have a condition.
      //
      if (vo.join == view_object::cross)
      {
        error (l)
          << "no join condition can be specified for a cross join" << endl;
        return;
      }

      tt = l.next (tl, &tn);

      if (!parse_expression (l, tt, tl, tn, vo.cond, p))
        return; // Diagnostics has already been issued.

      if (vo.cond.empty ())
      {
        error (l) << "join condition expected after ':' in db pragma " << p
                  << endl;
        return;
      }
    }

    if (tt != CPP_CLOSE_PAREN)
    {
      error (l) << "')' expected at the end of db pragma " << p << endl;
      return;
    }

    // Add the "table" pragma.
    //
    if (vo.alias.empty () && vo.cond.empty ())
    {
      if (db.empty () || db == pragma_db_.string ())
      {
        add_pragma (
          pragma (p, name, vo.tbl_name, loc, &check_spec_decl_type, 0),
          decl,
          ns);
      }
    }

    vo.scope = current_scope ();
    vo.loc = loc;
    val = vo;
    name = "objects";
    adder = &accumulate<view_object>;

    tt = l.next (tl, &tn);
  }
  else if (p == "session")
  {
    // session
    // session (true|false)
    //

    // Make sure we've got the correct declaration type.
    //
    if (decl && !check_spec_decl_type (decl, decl_name, p, loc))
      return;

    tt = l.next (tl, &tn);

    if (tt == CPP_OPEN_PAREN)
    {
      tt = l.next (tl, &tn);

      if (tt != CPP_KEYWORD || (tl != "true" && tl != "false"))
      {
        error (l) << "true or false expected in db pragma " << p << endl;
        return;
      }

      val = (tl == "true");

      tt = l.next (tl, &tn);
      if (tt != CPP_CLOSE_PAREN)
      {
        error (l) << "')' expected at the end of db pragma " << p << endl;
        return;
      }

      tt = l.next (tl, &tn);
    }
  }
  else if (p == "schema")
  {
    // schema (<name>)
    //
    // <name> := "name" | "name.name" | "name"."name"
    //

    // Make sure we've got the correct declaration type.
    //
    if (decl && !check_spec_decl_type (decl, decl_name, p, loc))
      return;

    if (l.next (tl, &tn) != CPP_OPEN_PAREN)
    {
      error (l) << "'(' expected after db pragma " << p << endl;
      return;
    }

    tt = l.next (tl, &tn);

    if (tt != CPP_STRING && tt != CPP_DOT)
    {
      error (l) << "table name expected in db pragma " << p << endl;
      return;
    }

    qname s;
    if (!parse_qname (l, tt, tl, tn, p, s))
      return; // Diagnostics has already been issued.

    if (tt != CPP_CLOSE_PAREN)
    {
      error (l) << "')' expected at the end of db pragma " << p << endl;
      return;
    }

    val = s;
    tt = l.next (tl, &tn);
  }
  else if (p == "pointer")
  {
    // pointer (qname)
    //

    // Make sure we've got the correct declaration type.
    //
    if (decl && !check_spec_decl_type (decl, decl_name, p, loc))
      return;

    if (l.next (tl, &tn) != CPP_OPEN_PAREN)
    {
      error (l) << "'(' expected after db pragma " << p << endl;
      return;
    }

    class_pointer cp;
    size_t pb (0);
    bool punc (false);

    for (tt = l.next (tl, &tn);
         tt != CPP_EOF && (tt != CPP_CLOSE_PAREN || pb != 0);
         tt = l.next (tl, &tn))
    {
      if (punc && tt > CPP_LAST_PUNCTUATOR)
        cp.name += ' ';

      punc = false;

      if (tt == CPP_OPEN_PAREN)
        pb++;
      else if (tt == CPP_CLOSE_PAREN)
        pb--;

      // @@ Need to handle literals, at least integer.
      //
      switch (tt)
      {
      case CPP_LESS:
        {
          cp.name += "< ";
          break;
        }
      case CPP_GREATER:
        {
          cp.name += " >";
          break;
        }
      case CPP_COMMA:
        {
          cp.name += ", ";
          break;
        }
      case CPP_NAME:
      // case CPP_KEYWORD: // see default:
        {
          cp.name += tl;
          punc = true;
          break;
        }
      default:
        {
          if (tt == CPP_KEYWORD)
          {
            cp.name += tl;
            punc = true;
          }
          else if (tt <= CPP_LAST_PUNCTUATOR)
            cp.name += cxx_lexer::token_spelling[tt];
          else
          {
            error (l) << "unexpected token '" << cxx_lexer::token_spelling[tt]
                      << "' in db pragma " << p << endl;
            return;
          }
          break;
        }
      }
    }

    if (tt != CPP_CLOSE_PAREN)
    {
      error (l) << "')' expected at the end of db pragma " << p << endl;
      return;
    }

    if (cp.name.empty ())
    {
      error (l) << "expected pointer name in db pragma " << p << endl;
      return;
    }

    cp.scope = current_scope ();
    cp.loc = loc;
    val = cp;

    tt = l.next (tl, &tn);
  }
  else if (p == "abstract")
  {
    // abstract
    //

    // Make sure we've got the correct declaration type.
    //
    if (decl && !check_spec_decl_type (decl, decl_name, p, loc))
      return;

    tt = l.next (tl, &tn);
  }
  else if (p == "optimistic")
  {
    // optimistic
    //

    // Make sure we've got the correct declaration type.
    //
    if (decl && !check_spec_decl_type (decl, decl_name, p, loc))
      return;

    tt = l.next (tl, &tn);
  }
  else if (p == "polymorphic")
  {
    // polymorphic
    //

    // Make sure we've got the correct declaration type.
    //
    if (decl && !check_spec_decl_type (decl, decl_name, p, loc))
      return;

    tt = l.next (tl, &tn);
  }
  else if (p == "definition")
  {
    // definition
    //

    // Make sure we've got the correct declaration type.
    //
    if (decl && !check_spec_decl_type (decl, decl_name, p, loc))
      return;

    val = l.location ();
    tt = l.next (tl, &tn);
  }
  else if (p == "sectionable")
  {
    // sectionable
    //

    // Make sure we've got the correct declaration type.
    //
    if (decl && !check_spec_decl_type (decl, decl_name, p, loc))
      return;

    tt = l.next (tl, &tn);
  }
  else if (p == "callback")
  {
    // callback (name)
    //

    // Make sure we've got the correct declaration type.
    //
    if (decl && !check_spec_decl_type (decl, decl_name, p, loc))
      return;

    if (l.next (tl, &tn) != CPP_OPEN_PAREN)
    {
      error (l) << "'(' expected after db pragma " << p << endl;
      return;
    }

    tt = l.next (tl, &tn);

    if (tt != CPP_NAME)
    {
      error (l) << "member function name expected in db pragma " << p << endl;
      return;
    }

    val = tl;

    if (l.next (tl, &tn) != CPP_CLOSE_PAREN)
    {
      error (l) << "')' expected at the end of db pragma " << p << endl;
      return;
    }

    tt = l.next (tl, &tn);
  }
  else if (p == "bulk")
  {
    // bulk (batch-size)
    //

    // Make sure we've got the correct declaration type.
    //
    if (decl && !check_spec_decl_type (decl, decl_name, p, loc))
      return;

    if (l.next (tl, &tn) != CPP_OPEN_PAREN)
    {
      error (l) << "'(' expected after db pragma " << p << endl;
      return;
    }

    if (l.next (tl, &tn) != CPP_NUMBER || TREE_CODE (tn) != INTEGER_CST)
    {
      error (l) << "unsigned integer expected as batch size" << endl;
      return;
    }

    unsigned long long b (integer_value (tn));

    if (b == 0 || b == 1)
    {
      error (l) << "batch size has to be greater than 1" << endl;
      return;
    }

    val = b;

    if (l.next (tl, &tn) != CPP_CLOSE_PAREN)
    {
      error (l) << "')' expected at the end of db pragma " << p << endl;
      return;
    }

    tt = l.next (tl, &tn);
  }
  else if (p == "query")
  {
    // query ()
    // query ("statement")
    // query (expr)
    //

    // Make sure we've got the correct declaration type.
    //
    if (decl && !check_spec_decl_type (decl, decl_name, p, loc))
      return;

    if (l.next (tl, &tn) != CPP_OPEN_PAREN)
    {
      error (l) << "'(' expected after db pragma " << p << endl;
      return;
    }

    tt = l.next (tl, &tn);

    view_query vq;

    bool s (false);
    string str;

    if (tt == CPP_STRING)
    {
      s = true;
      str = tl;
      tt = l.next (tl, &tn);
    }

    if (tt == CPP_CLOSE_PAREN)
    {
      if (s)
        vq.literal = str;
      else
      {
        // Empty query() pragma indicates that the statement will be
        // provided at runtime. Encode this case as empty literal
        // and expression.
        //
      }
    }
    else
    {
      // Expression.
      //
      if (s)
        vq.expr.push_back (cxx_token (0, CPP_STRING, str));

      if (!parse_expression (l, tt, tl, tn, vq.expr, p))
        return; // Diagnostics has already been issued.
    }

    // Disallow query(, distinct).
    //
    if (tt == CPP_COMMA && vq.expr.empty ())
    {
      error (l) << "query expression expected in db pragma " << p << endl;
      return;
    }

    // The query expression can be omitted with the modifier in its
    // place. Handle this case.
    //
    if (vq.expr.size () == 1 && vq.expr.front ().type == CPP_NAME)
    {
      string const& n (vq.expr.front ().literal);

      if (n == "distinct")
        vq.distinct = true;
      else if (n == "for_update")
        vq.for_update = true;

      if (vq.distinct || vq.for_update)
        vq.expr.clear ();
    }

    if (tt == CPP_COMMA)
    {
      do
      {
        if (l.next (tl, &tn) != CPP_NAME)
        {
          error (l) << "result modifier expected in db pragma " << p << endl;
          return;
        }

        if (tl == "distinct")
          vq.distinct = true;
        else if (tl == "for_update")
          vq.for_update = true;
        else
        {
          error (l) << "unknown result modifier '" << tl << "'" << endl;
          return;
        }

        tt = l.next (tl, &tn);

      } while (tt == CPP_COMMA);
    }

    if (tt != CPP_CLOSE_PAREN)
    {
      error (l) << "')' expected at the end of db pragma " << p << endl;
      return;
    }

    vq.scope = current_scope ();
    vq.loc = loc;
    val = vq;
    tt = l.next (tl, &tn);
  }
  else if (p == "object")
  {
    // object (fq-name [ = name] [type] [: expr])
    //

    // Make sure we've got the correct declaration type.
    //
    if (decl && !check_spec_decl_type (decl, decl_name, p, loc))
      return;

    if (l.next (tl, &tn) != CPP_OPEN_PAREN)
    {
      error (l) << "'(' expected after db pragma " << p << endl;
      return;
    }

    tt = l.next (tl, &tn);

    if (tt != CPP_NAME && tt != CPP_SCOPE)
    {
      error (l) << "type name expected in db pragma " << p << endl;
      return;
    }

    view_object vo;
    vo.kind = view_object::object;
    vo.obj_node = resolve_scoped_name (
      l, tt, tl, tn, current_scope (), vo.obj_name, true, p);

    if (vo.obj_node == 0)
      return; // Diagnostics has already been issued.

    // Get the actual type if this is a TYPE_DECL. Also get the main variant.
    //
    if (TREE_CODE (vo.obj_node) == TYPE_DECL)
      vo.obj_node = TREE_TYPE (vo.obj_node);

    if (TYPE_P (vo.obj_node)) // Can be a template.
      vo.obj_node = TYPE_MAIN_VARIANT (vo.obj_node);

    if (tt == CPP_EQ)
    {
      // We have an alias.
      //
      if (l.next (tl, &tn) != CPP_NAME)
      {
        error (l) << "alias name expected after '=' in db pragma " << p
                  << endl;
        return;
      }

      vo.alias = tl;
      tt = l.next (tl, &tn);
    }

    if (tt == CPP_NAME)
    {
      // We have a JOIN type.
      //
      if (tl == "left")
        vo.join = view_object::left;
      else if (tl == "right")
        vo.join = view_object::right;
      else if (tl == "full")
        vo.join = view_object::full;
      else if (tl == "inner")
        vo.join = view_object::inner;
      else if (tl == "cross")
        vo.join = view_object::cross;
      else
      {
        error (l) << "unknown join type '" << tl << "'" << endl;
        return;
      }

      tt = l.next (tl, &tn);
    }
    else
      vo.join = view_object::left;

    if (tt == CPP_COLON)
    {
      // We have a condition.
      //
      if (vo.join == view_object::cross)
      {
        error (l)
          << "no join condition can be specified for a cross join" << endl;
        return;
      }

      tt = l.next (tl, &tn);

      if (!parse_expression (l, tt, tl, tn, vo.cond, p))
        return; // Diagnostics has already been issued.

      if (vo.cond.empty ())
      {
        error (l) << "join condition expected after ':' in db pragma " << p
                  << endl;
        return;
      }
    }

    if (tt != CPP_CLOSE_PAREN)
    {
      error (l) << "')' expected at the end of db pragma " << p << endl;
      return;
    }

    vo.scope = current_scope ();
    vo.loc = loc;
    val = vo;
    name = "objects"; // Change the context entry name.
    adder = &accumulate<view_object>;

    tt = l.next (tl, &tn);
  }
  else if (p == "id")
  {
    // id[(member-path)]
    //

    // Make sure we've got the correct declaration type.
    //
    if (decl && !check_spec_decl_type (decl, decl_name, p, loc))
      return;

    string name;

    tt = l.next (tl, &tn);
    if (tt == CPP_OPEN_PAREN)
    {
      if (l.next (tl, &tn) != CPP_NAME)
      {
        error (l) << "data member name expected in db pragma " << p
                  << endl;
        return;
      }

      name = tl;

      for (tt = l.next (tl, &tn); tt == CPP_DOT; tt = l.next (tl, &tn))
      {
        if (l.next (tl, &tn) != CPP_NAME)
        {
          error (l) << "name expected after '.' in db pragma " << p << endl;
          return;
        }

        name += '.';
        name += tl;
      }

      if (tt != CPP_CLOSE_PAREN)
      {
        error (l) << "')' expected at the end of db pragma " << p << endl;
        return;
      }

      tt = l.next (tl, &tn);
    }

    val = name;
  }
  else if (p == "no_id")
  {
    // Make sure we've got the correct declaration type.
    //
    if (decl && !check_spec_decl_type (decl, decl_name, p, loc))
      return;

    name = "id";
    val = false;

    tt = l.next (tl, &tn);
  }
  else if (p == "auto")
  {
    // auto
    //

    // Make sure we've got the correct declaration type.
    //
    if (decl && !check_spec_decl_type (decl, decl_name, p, loc))
      return;

    tt = l.next (tl, &tn);
  }
  else if (p == "column")
  {
    // column ("<name>")
    // column ("<name>.<name>")     (view only)
    // column ("<name>"."<name>")   (view only)
    // column (fq-name)             (view only)
    // column (expr)                (view only)
    //

    // Make sure we've got the correct declaration type.
    //
    if (decl && !check_spec_decl_type (decl, decl_name, p, loc))
      return;

    if (l.next (tl, &tn) != CPP_OPEN_PAREN)
    {
      error (l) << "'(' expected after db pragma " << p << endl;
      return;
    }

    tt = l.next (tl, &tn);

    bool expr (false);
    string expr_str;
    if (tt == CPP_STRING || tt == CPP_DOT)
    {
      qname qn;

      if (!parse_qname (l, tt, tl, tn, p, qn, &expr, &expr_str))
        return; // Diagnostics has already been issued.

      if (tt == CPP_CLOSE_PAREN)
      {
        table_column tc;
        tc.expr = expr;

        if (expr)
          tc.column = expr_str;
        else
        {
          tc.table = qn.qualifier ();
          tc.column = qn.uname ();
        }

        val = tc;
      }
      else if (!expr)
      {
        error (l) << "column name, expression, or data member name expected "
                  << "in db pragma " << p << endl;
        return;
      }
    }

    if (val.empty ())
    {
      // We have an expression.
      //
      column_expr e;

      if (expr)
      {
        e.push_back (column_expr_part ());
        e.back ().kind = column_expr_part::literal;
        e.back ().value = expr_str;

        if (tt != CPP_PLUS)
        {
          error (l) << "'+' or ')' expected in db pragma " << p << endl;
          return;
        }

        tt = l.next (tl, &tn);
      }

      for (;;)
      {
        if (tt == CPP_STRING)
        {
          e.push_back (column_expr_part ());
          e.back ().kind = column_expr_part::literal;
          e.back ().value = tl;

          tt = l.next (tl, &tn);
        }
        else if (tt == CPP_NAME || tt == CPP_SCOPE)
        {
          string name (parse_scoped_name (l, tt, tl, tn, p));

          if (name.empty ())
            return; // Diagnostics has already been issued.

          // Resolve nested members if any.
          //
          for (; tt == CPP_DOT; tt = l.next (tl, &tn))
          {
            if (l.next (tl, &tn) != CPP_NAME)
            {
              error (l) << "name expected after '.' in db pragma " << p
                        << endl;
              return;
            }

            name += '.';
            name += tl;
          }

          e.push_back (column_expr_part ());
          e.back ().kind = column_expr_part::reference;
          e.back ().value = name;
          e.back ().scope = current_scope ();
          e.back ().loc = loc;
        }
        else
        {
          error (l) << "column name, expression, or data member name expected "
                    << "in db pragma " << p << endl;
          return;
        }

        if (tt == CPP_PLUS)
          tt = l.next (tl, &tn);
        else if (tt == CPP_CLOSE_PAREN)
          break;
        else
        {
          error (l) << "'+' or ')' expected in db pragma " << p << endl;
          return;
        }
      }

      e.loc = loc;
      val = e;
      name = "column-expr";
    }

    tt = l.next (tl, &tn);
  }
  else if (p == "value_column" ||
           p == "index_column" ||
           p == "key_column" ||
           p == "id_column")
  {
    // value_column ("<name>")
    // index_column ("<name>")
    // key_column ("<name>")
    // id_column ("<name>")
    //

    // Make sure we've got the correct declaration type.
    //
    if (decl && !check_spec_decl_type (decl, decl_name, p, loc))
      return;

    if (l.next (tl, &tn) != CPP_OPEN_PAREN)
    {
      error (l) << "'(' expected after db pragma " << p << endl;
      return;
    }

    tt = l.next (tl, &tn);

    if (tt != CPP_STRING)
    {
      error (l) << "column name expected in db pragma " << p << endl;
      return;
    }

    val = tl;

    if (l.next (tl, &tn) != CPP_CLOSE_PAREN)
    {
      error (l) << "')' expected at the end of db pragma " << p << endl;
      return;
    }

    tt = l.next (tl, &tn);
  }
  else if (p == "options" ||
           p == "value_options" ||
           p == "index_options" ||
           p == "key_options" ||
           p == "id_options")
  {
    // options (["<name>"])
    // value_options (["<name>"])
    // index_options (["<name>"])
    // key_options (["<name>"])
    // id_options (["<name>"])
    //

    // Make sure we've got the correct declaration type.
    //
    if (decl && !check_spec_decl_type (decl, decl_name, p, loc))
      return;

    if (l.next (tl, &tn) != CPP_OPEN_PAREN)
    {
      error (l) << "'(' expected after db pragma " << p << endl;
      return;
    }

    tt = l.next (tl, &tn);

    if (tt == CPP_STRING)
    {
      // Ignore empty options strings. Internally, we use them to
      // indicate options reset (see below).
      //
      if (!tl.empty ())
        val = tl;

      tt = l.next (tl, &tn);
    }
    else if (tt == CPP_CLOSE_PAREN)
    {
      // Empty options specifier signals options reset. Encode it as an
      // empty string.
      //
      val = string ();
    }
    else
    {
      error (l) << "options string expected in db pragma " << p << endl;
      return;
    }

    if (tt != CPP_CLOSE_PAREN)
    {
      error (l) << "')' expected at the end of db pragma " << p << endl;
      return;
    }

    adder = &accumulate<string>;
    tt = l.next (tl, &tn);
  }
  else if (p == "type" ||
           p == "id_type" ||
           p == "value_type" ||
           p == "index_type" ||
           p == "key_type")
  {
    // type ("<type>")
    // id_type ("<type>")
    // value_type ("<type>")
    // index_type ("<type>")
    // key_type ("<type>")
    //

    // Make sure we've got the correct declaration type.
    //
    if (decl && !check_spec_decl_type (decl, decl_name, p, loc))
      return;

    if (l.next (tl, &tn) != CPP_OPEN_PAREN)
    {
      error (l) << "'(' expected after db pragma " << p << endl;
      return;
    }

    tt = l.next (tl, &tn);

    if (tt != CPP_STRING)
    {
      error (l) << "type name expected in db pragma " << p << endl;
      return;
    }

    val = tl;

    if (l.next (tl, &tn) != CPP_CLOSE_PAREN)
    {
      error (l) << "')' expected at the end of db pragma " << p << endl;
      return;
    }

    tt = l.next (tl, &tn);
  }
  else if (p == "null" ||
           p == "not_null" ||
           p == "key_null" ||
           p == "key_not_null" ||
           p == "value_null" ||
           p == "value_not_null")
  {
    // null
    // not_null
    // key_null
    // key_not_null
    // value_null
    // value_not_null
    //

    // Make sure we've got the correct declaration type.
    //
    if (decl && !check_spec_decl_type (decl, decl_name, p, loc))
      return;

    tt = l.next (tl, &tn);
  }
  else if (p == "default")
  {
    // default ()                  (<empty>)
    // default (null)              (n)
    // default (true|false)        (t|f)
    // default ([+|-]<number>)     (-|+)
    // default ("string")          (s)
    // default (<enumerator>)      (e)
    //
    //

    // Make sure we've got the correct declaration type.
    //
    if (decl && !check_spec_decl_type (decl, decl_name, p, loc))
      return;

    if (l.next (tl, &tn) != CPP_OPEN_PAREN)
    {
      error (l) << "'(' expected after db pragma " << p << endl;
      return;
    }

    tt = l.next (tl, &tn);

    default_value dv;

    switch (tt)
    {
    case CPP_CLOSE_PAREN:
      {
        // Default value reset.
        //
        dv.kind = default_value::reset;
        break;
      }
    case CPP_STRING:
      {
        dv.kind = default_value::string;
        dv.literal = tl;
        tt = l.next (tl, &tn);
        break;
      }
    case CPP_NAME:
      {
        // This can be null or an enumerator name.
        //
        if (tl == "null")
        {
          dv.kind = default_value::null;
          tt = l.next (tl, &tn);
          break;
        }
      }
      // Fall through.
    case CPP_SCOPE:
      {
        // We have a potentially scopped enumerator name.
        //
        dv.enum_value = resolve_scoped_name (
          l, tt, tl, tn, current_scope (), dv.literal, false, p);

        if (dv.enum_value == 0)
          return; // Diagnostics has already been issued.

        dv.kind = default_value::enumerator;
        break;
      }
    case CPP_MINUS:
    case CPP_PLUS:
      {
        if (tt == CPP_MINUS)
          dv.literal = "-";

        tt = l.next (tl, &tn);

        if (tt != CPP_NUMBER)
        {
          error (l) << "expected numeric constant after '"
                    << (tt == CPP_MINUS ? "-" : "+") << "' in db pragma "
                    << p << endl;
          return;
        }
      }
      // Fall through.
    case CPP_NUMBER:
      {
        switch (TREE_CODE (tn))
        {
        case INTEGER_CST:
          {
            dv.int_value = integer_value (tn);
            dv.kind = default_value::integer;
            break;
          }
        case REAL_CST:
          {
            dv.float_value = static_cast<tree_real_cst const*> (
              tn.ptr ())->value;

            if (dv.literal == "-")
              dv.float_value = -dv.float_value;

            dv.kind = default_value::floating;
            break;
          }
        default:
          {
            error (l) << "unexpected numeric constant in db pragma " << p
                      << endl;
            return;
          }
        }

        tt = l.next (tl, &tn);
        break;
      }
    default:
      {
        // This can be the true or false keyword.
        //
        if (tt == CPP_KEYWORD && (tl == "true" || tl == "false"))
        {
          dv.kind = default_value::boolean;
          dv.literal = tl;
          tt = l.next (tl, &tn);
        }
        else
        {
          error (l) << "unexpected expression in db pragma " << p << endl;
          return;
        }

        break;
      }
    }

    if (tt != CPP_CLOSE_PAREN)
    {
      error (l) << "')' expected at the end of db pragma " << p << endl;
      return;
    }

    val = dv;
    tt = l.next (tl, &tn);
  }
  else if (p == "inverse")
  {
    // inverse (name)
    //

    // Make sure we've got the correct declaration type.
    //
    if (decl && !check_spec_decl_type (decl, decl_name, p, loc))
      return;

    if (l.next (tl, &tn) != CPP_OPEN_PAREN)
    {
      error (l) << "'(' expected after db pragma " << p << endl;
      return;
    }

    tt = l.next (tl, &tn);

    if (tt != CPP_NAME)
    {
      error (l) << "member name expected in db pragma " << p << endl;
      return;
    }

    string name (tl);

    tt = l.next (tl, &tn);

    // Parse nested members if any.
    //
    for (; tt == CPP_DOT; tt = l.next (tl, &tn))
    {
      if (l.next (tl, &tn) != CPP_NAME)
      {
        error (l) << "name expected after '.' in db pragma " << p << endl;
        return;
      }

      name += '.';
      name += tl;
    }

    if (tt != CPP_CLOSE_PAREN)
    {
      error (l) << "')' expected at the end of db pragma " << p << endl;
      return;
    }

    val = name;
    tt = l.next (tl, &tn);
  }
  else if (p == "on_delete")
  {
    // on_delete (cascade|set_null)
    //

    // Make sure we've got the correct declaration type.
    //
    if (decl && !check_spec_decl_type (decl, decl_name, p, loc))
      return;

    if (l.next (tl, &tn) != CPP_OPEN_PAREN)
    {
      error (l) << "'(' expected after db pragma " << p << endl;
      return;
    }

    if (l.next (tl, &tn) != CPP_NAME || (tl != "cascade" && tl != "set_null"))
    {
      error (l) << "cascade or set_null expected after '('" << endl;
      return;
    }

    using semantics::relational::foreign_key;
    val = (tl == "cascade" ? foreign_key::cascade : foreign_key::set_null);

    if (l.next (tl, &tn) != CPP_CLOSE_PAREN)
    {
      error (l) << "')' expected at the end of db pragma " << p << endl;
      return;
    }

    tt = l.next (tl, &tn);
  }
  else if (p == "points_to")
  {
    // points_to(<fq-name>)
    //

    tree type;
    string type_name;

    string p (tl);
    location_t loc (l.location ());

    if (l.next (tl, &tn) != CPP_OPEN_PAREN)
    {
      error (l) << "'(' expected after db pragma " << p << endl;
      return;
    }

    tt = l.next (tl, &tn);

    if (tt == CPP_NAME || tt == CPP_SCOPE)
    {
      type = resolve_scoped_name (
        l, tt, tl, tn, current_scope (), type_name, true, p);

      if (type == 0)
        return; // Diagnostics has already been issued.

      // Get the actual type if this is a TYPE_DECL. Also get the main
      // variant.
      //
      if (TREE_CODE (type) == TYPE_DECL)
        type = TREE_TYPE (type);

      if (TYPE_P (type)) // Can be a template.
        type = TYPE_MAIN_VARIANT (type);

      if (TREE_CODE (type) != RECORD_TYPE)
      {
        error (loc) << "name '" << type_name << "' in db pragma " << p
                    << " does not refer to a class" << endl;
        return;
      }

      val = type;
    }
    else
    {
      error (l) << "class name expected in db pragma " << p << endl;
      return;
    }

    if (tt != CPP_CLOSE_PAREN)
    {
      error (l) << "')' expected at the end of db pragma " << p << endl;
      return;
    }

    tt = l.next (tl, &tn);
  }
  else if (p == "section")
  {
    // section (name)
    //

    // Make sure we've got the correct declaration type.
    //
    if (decl && !check_spec_decl_type (decl, decl_name, p, loc))
      return;

    if (l.next (tl, &tn) != CPP_OPEN_PAREN)
    {
      error (l) << "'(' expected after db pragma " << p << endl;
      return;
    }

    tt = l.next (tl, &tn);

    if (tt != CPP_NAME)
    {
      error (l) << "member name expected in db pragma " << p << endl;
      return;
    }

    name = "section-member";
    val = tl;

    if (l.next (tl, &tn) != CPP_CLOSE_PAREN)
    {
      error (l) << "')' expected at the end of db pragma " << p << endl;
      return;
    }

    tt = l.next (tl, &tn);
  }
  else if (p == "load")
  {
    // load (eager|lazy)
    //

    // Make sure we've got the correct declaration type.
    //
    if (decl && !check_spec_decl_type (decl, decl_name, p, loc))
      return;

    if (l.next (tl, &tn) != CPP_OPEN_PAREN)
    {
      error (l) << "'(' expected after db pragma " << p << endl;
      return;
    }

    tt = l.next (tl, &tn);

    if (tt != CPP_NAME || (tl != "eager" && tl != "lazy"))
    {
      error (l) << "eager or lazy expected in db pragma " << p << endl;
      return;
    }

    name = "section-load";
    val = (tl == "eager"
           ? user_section::load_eager
           : user_section::load_lazy);

    if (l.next (tl, &tn) != CPP_CLOSE_PAREN)
    {
      error (l) << "')' expected at the end of db pragma " << p << endl;
      return;
    }

    tt = l.next (tl, &tn);
  }
  else if (p == "update")
  {
    // update (always|change|manual)
    //

    // Make sure we've got the correct declaration type.
    //
    if (decl && !check_spec_decl_type (decl, decl_name, p, loc))
      return;

    if (l.next (tl, &tn) != CPP_OPEN_PAREN)
    {
      error (l) << "'(' expected after db pragma " << p << endl;
      return;
    }

    tt = l.next (tl, &tn);

    if (tt != CPP_NAME ||
        (tl != "always" && tl != "change" && tl != "manual"))
    {
      error (l) << "always, change, or manual expected in db pragma " <<
        p << endl;
      return;
    }

    name = "section-update";

    if (tl == "always")
      val = user_section::update_always;
    else if (tl == "change")
      val = user_section::update_change;
    else
      val = user_section::update_manual;

    if (l.next (tl, &tn) != CPP_CLOSE_PAREN)
    {
      error (l) << "')' expected at the end of db pragma " << p << endl;
      return;
    }

    tt = l.next (tl, &tn);
  }
  else if (p == "unordered")
  {
    // unordered
    //

    // Make sure we've got the correct declaration type.
    //
    if (decl && !check_spec_decl_type (decl, decl_name, p, loc))
      return;

    tt = l.next (tl, &tn);
  }
  else if (p == "readonly")
  {
    // readonly
    //

    // Make sure we've got the correct declaration type.
    //
    if (decl && !check_spec_decl_type (decl, decl_name, p, loc))
      return;

    tt = l.next (tl, &tn);
  }
  else if (p == "transient")
  {
    // transient
    //

    // Make sure we've got the correct declaration type.
    //
    if (decl && !check_spec_decl_type (decl, decl_name, p, loc))
      return;

    tt = l.next (tl, &tn);
  }
  else if (p == "added" || p == "deleted")
  {
    // added (unsigned long long version)
    // deleted (unsigned long long version)
    //

    // Make sure we've got the correct declaration type.
    //
    if (decl && !check_spec_decl_type (decl, decl_name, p, loc))
      return;

    char const* n (p == "added" ? "addition" : "deletion");

    if (l.next (tl, &tn) != CPP_OPEN_PAREN)
    {
      error (l) << "'(' expected after db pragma " << p << endl;
      return;
    }

    if (l.next (tl, &tn) != CPP_NUMBER || TREE_CODE (tn) != INTEGER_CST)
    {
      error (l) << "unsigned integer expected as " << n << " version" << endl;
      return;
    }

    unsigned long long v (integer_value (tn));

    if (v == 0)
    {
      error (l) << n << " version cannot be zero" << endl;
      return;
    }

    if (l.next (tl, &tn) != CPP_CLOSE_PAREN)
    {
      error (l) << "')' expected at the end of db pragma " << p << endl;
      return;
    }

    val = v;
    tt = l.next (tl, &tn);
  }
  else if (p == "version")
  {
    // version
    //

    // Make sure we've got the correct declaration type.
    //
    if (decl && !check_spec_decl_type (decl, decl_name, p, loc))
      return;

    tt = l.next (tl, &tn);
  }
  else if (p == "direct_load" || p == "indirect_load")
  {
    // direct_load
    // indirect_load
    //

    // Make sure we've got the correct declaration type.
    //
    if (decl && !check_spec_decl_type (decl, decl_name, p, loc))
      return;

    // Reduce to direct-load=true|false.
    //
    name = "direct-load";
    val = (p == "direct_load");

    tt = l.next (tl, &tn);
  }
  else if (p == "virtual")
  {
    // Stray virtual specifier (i.e., without explicit member name).
    //
    error (l) << "virtual member declaration requires name" << endl;
    return;
  }
  else if (p == "before" || p == "after")
  {
    // Stray before/after specifier (i.e., without preceding virtual).
    //
    error (l) << p << " specifier must follow virtual" << endl;
    return;
  }
  else
  {
    error (l) << "unknown db pragma " << p << endl;
    return;
  }

  // Add the pragma unless was indicated otherwise.
  //
  if (!name.empty () && (db.empty () || db == pragma_db_.string ()))
  {
    // If the value is not specified and we don't use a custom adder,
    // then make it bool (flag).
    //
    if (adder == 0 && val.empty ())
      val = true;

    // Convert '_' to '-' in the context name so we get foo-bar instead
    // of foo_bar (that's the convention used).
    //
    for (size_t i (0); i < name.size (); ++i)
      if (name[i] == '_')
        name[i] = '-';

    // Record this pragma.
    //
    add_pragma (
      pragma (p, name, val, loc, &check_spec_decl_type, adder), decl, ns);
  }

  // Mark the type or member as simple value or container, depending
  // on the pragma. For static multi-database support we only do it
  // if the pragma applies to this database since in this case we can
  // have different mappings for different databases (e.g., composite
  // in one and simple in another). For dynamic multi-database support
  // we do this regardless of the database since here the mapping
  // should the consistent.
  //
  // @@ Did we add new simple value or container pragmas and forgot to
  //    account for them here?
  //
  if ((qualifier == "value" || qualifier == "member") &&
      (pragma_multi_ == multi_database::dynamic || db.empty () ||
       db == pragma_db_.string ()))
  {
    // We assume a data member is simple only if the database type was
    // specified explicitly.
    //
    if (name == "type"     ||
        name == "id-type"  ||
        (qualifier == "value" &&
         (name == "null"     ||
          name == "not-null" ||
          name == "default"  ||
          name == "options")))
    {
      add_pragma (pragma (p, "simple", true, loc, &check_spec_decl_type, 0),
                  decl,
                  false);
    }
    else if (name == "table"      ||
             name == "value-type" ||
             name == "index-type" ||
             name == "key-type"   ||

             name == "key-null"       ||
             name == "key-not-null"   ||
             name == "value-null"     ||
             name == "value-not-null" ||

             name == "value-column" ||
             name == "index-column" ||
             name == "key-column"   ||
             name == "index-column" ||

             name == "value-options" ||
             name == "index-options" ||
             name == "key-options"   ||
             name == "index-options" ||

             name == "unordered")
    {
      add_pragma (pragma (p, "container", true, loc, &check_spec_decl_type, 0),
                  decl,
                  false);
    }
  }

  // See if there are any more pragmas.
  //
  if (tt == CPP_NAME || tt == CPP_KEYWORD)
  {
    handle_pragma (l,
                   "",
                   tl,
                   qualifier,
                   qualifier_value,
                   decl,
                   decl_name,
                   ns);
  }
  else if (tt != CPP_EOF)
    error (l) << "unexpected text after " << p << " in db pragma" << endl;
}

//
// Qualifiers.
//

static bool
check_qual_decl_type (declaration const& d,
                      string const& name,
                      string const& p,
                      location_t l)
{
  gcc_tree_code_type tc (d.tree_code ());
  bool type (TREE_CODE_CLASS (tc) == tcc_type);

  if (p == "model" ||
      p == "map")
  {
    assert (d == global_namespace);
  }
  else if (p == "index")
  {
    if (tc != RECORD_TYPE)
    {
      // For an index, name is not empty only if the class name was
      // specified explicitly. Otherwise, the index definition scope
      // is assumed.
      //
      if (name.empty ())
      {
        error (l) << "db pragma " << p << " outside of a class scope" << endl;
        info (l) << "use the db pragma " << p << "(<class-name>) syntax "
                 << " instead" << endl;
      }
      else
        error (l) << "name '" << name << "' in db pragma " << p << " does "
                  << "not refer to a class" << endl;
      return false;
    }
  }
  else if (p == "namespace")
  {
    if (tc != NAMESPACE_DECL)
    {
      error (l) << "name '" << name << "' in db pragma " << p << " does "
                << "not refer to a namespace" << endl;
      return false;
    }
  }
  else if (p == "object" ||
           p == "view")
  {
    if (tc != RECORD_TYPE)
    {
      error (l) << "name '" << name << "' in db pragma " << p << " does "
                << "not refer to a class" << endl;
      return false;
    }
  }
  else if (p == "value")
  {
    if (!type)
    {
      error (l) << "name '" << name << "' in db pragma " << p << " does "
                << "not refer to a type" << endl;
      return false;
    }
  }
  else if (p == "member")
  {
    if (tc != FIELD_DECL)
    {
      error (l) << "name '" << name << "' in db pragma " << p << " does "
                << "not refer to a data member" << endl;
      return false;
    }
  }
  else
  {
    error (l) << "unknown db pragma " << p << endl;
    return false;
  }

  return true;
}

static void
add_qual_entry (compiler::context& ctx,
                string const& k,
                any const& v,
                location_t l)
{
  // Store the TYPE_DECL node that was referred to in the pragma. This
  // can be used later as a name hint in case the type is a template
  // instantiation. Also store the pragma location which is used as
  // the "definition point" for this instantiation.
  //
  ctx.set ("tree-node", v);
  ctx.set ("location", l);

  ctx.set (k, true);
}

static void
handle_pragma_qualifier (cxx_lexer& l, string p)
{
  cpp_ttype tt;
  string tl;
  tree tn;

  declaration decl;
  tree orig_decl (0);   // Original declarations as used in the pragma.
  string decl_name;

  // Check for a database prefix.
  //
  string db;

  if (p == "mysql"  ||
      p == "sqlite" ||
      p == "pgsql"  ||
      p == "oracle" ||
      p == "mssql")
  {
    tt = l.next (tl);

    if (tt == CPP_COLON)
    {
      // Specifier prefix.
      //
      db = p;
      tt = l.next (p);
    }
    else
    {
      // Qualifier prefix. Ignore the rest if the databases don't match.
      //
      if (p != pragma_db_.string ())
        return;

      p = tl;
    }

    if (tt != CPP_NAME && tt != CPP_KEYWORD)
    {
      error (l) << "expected specifier after db " << db << " pragma" << endl;
      return;
    }

    // Make sure a qualifier prefix is not used before a specifier.
    //
    if (!db.empty () &&
        (p == "model"     ||
         p == "map"       ||
         p == "namespace" ||
         p == "object"    ||
         p == "view"      ||
         p == "value"     ||
         p == "member"))
    {
      error (l) << "specifier prefix '" << db << ":' used before qualifier " <<
        p << endl;
      return;
    }
  }

  //
  //
  string name (p);                 // Pragma name.
  any val;                         // Pragma value.
  location_t loc (l.location ());  // Pragma location.
  pragma::add_func adder (0);      // Custom context adder.
  bool ns (false);                 // Namespace location pragma.

  cxx_tokens saved_tokens;         // Saved token sequence to be replayed.

  // Pragma qualifiers.
  //
  if (p == "model")
  {
    orig_decl = global_namespace;
    decl = declaration (orig_decl);
    tt = l.next (tl, &tn);
  }
  else if (p == "map")
  {
    // map type("<regex>") as("<subst>") [to("<subst>")] [from("<subst>")]
    // map type(<c++-type>) as(<c++-type>) [to(<expr>)] [from(<expr>)]
    //

    // The thing that determines whether this is a database or C++ type
    // mapping is what we have inside 'type'. So to handle this we are
    // going to pre-scan the pragma looking for 'type' and saving the
    // tokens. Once we determine what this is, we replay the saved
    // tokens to actually parse them.
    //

    // Determine what this is by scanning the pragma until we see
    // the 'type' qualifier or EOF.
    //
    bool db (true);

    bool done (false);
    size_t balance (0);
    for (tt = l.next (tl, &tn);
         !(done && balance == 0) && tt != CPP_EOF;
         tt = l.next (tl, &tn))
    {
      saved_tokens.push_back (cxx_token (l.location (), tt, tl, tn));

      switch (tt)
      {
      case CPP_OPEN_PAREN:
        {
          balance++;
          continue;
        }
      case CPP_CLOSE_PAREN:
        {
          if (balance > 0)
            balance--;
          else
          {
            error (l) << "unbalanced parenthesis in db pragma " << p << endl;
            return;
          }
          continue;
        }
      case CPP_NAME:
        {
          if (balance == 0 && tl == "type")
            break;

          continue;
        }
      default:
        continue;
      }

      tt = l.next (tl, &tn);
      saved_tokens.push_back (cxx_token (l.location (), tt, tl, tn));

      if (tt == CPP_OPEN_PAREN)
      {
        balance++;

        tt = l.next (tl, &tn);
        saved_tokens.push_back (cxx_token (l.location (), tt, tl, tn));

        db = (tt == CPP_STRING);
      }

      done = true; // Scan until the closing ')'.
    }

    if (balance != 0)
    {
      error (l) << "unbalanced parenthesis in db pragma " << p << endl;
      return;
    }

    orig_decl = global_namespace;
    decl = declaration (orig_decl);

    if (db)
    {
      using relational::custom_db_type;

      custom_db_type ct;
      ct.loc = loc;
      val = ct;
      name = "custom-db-types";
      adder = &accumulate<custom_db_type>;
    }
    else
    {
      custom_cxx_type ct;
      ct.loc = loc;
      ct.scope = current_scope ();
      val = ct;
      name = "custom-cxx-types";
      adder = &accumulate<custom_cxx_type>;
    }
  }
  else if (p == "index")
  {
    // Index can be both a qualifier and a specifier. Things are complicated
    // by the fact that when it is a specifier, it belongs to a member which
    // means that the actual qualifier ('member') can be omitted. So we need
    // to distinguish between cases like these:
    //
    // #pragma db index type("INTEGER")              // specifier
    // #pragma db index type("UNIQUE") member(foo_)  // qualifier
    //
    // The thing that determines whether this is a qualifier or a specifier
    // is the presence of the 'member' or 'members' specifier. So to handle
    // this we are going to pre-scan the pragma looking for 'member' or
    // 'members' and saving the tokens. Once we determine what this is,
    // we replay the saved tokens to actually parse them.
    //
    tt = l.next (tl, &tn);

    if (tt != CPP_OPEN_PAREN)
    {
      // Determine what this is by scanning the pragma until we see
      // the 'member' qualifier or EOF.
      //
      bool qual (false);
      size_t balance (0);

      for (; tt != CPP_EOF; tt = l.next (tl, &tn))
      {
        switch (tt)
        {
        case CPP_OPEN_PAREN:
          {
            balance++;
            break;
          }
        case CPP_CLOSE_PAREN:
          {
            if (balance > 0)
              balance--;
            else
            {
              error (l) << "unbalanced parenthesis in db pragma " << p << endl;
              return;
            }
            break;
          }
        case CPP_NAME:
          {
            if (balance == 0 && (tl == "member" || tl == "members"))
              qual = true;
            break;
          }
        default:
          break;
        }

        if (qual)
          break;

        saved_tokens.push_back (cxx_token (l.location (), tt, tl, tn));
      }

      if (balance != 0)
      {
        error (l) << "unbalanced parenthesis in db pragma " << p << endl;
        return;
      }

      if (qual)
      {
        // This is a qualifer. The saved tokens sequence contains tokens
        // until the first 'member' or 'members' specifier. So we will
        // first need to re-play these tokens and then continue parsing
        // as if we just saw the 'member' or 'members' specifier. The
        // token type (tt) and token literal (tl) variables should contain
        // the correct values.
        //

        // Also check that no specifier prefix was used for this qualifer.
        //
        if (!db.empty ())
        {
          error (loc) << "specifier prefix '" << db << ":' used before " <<
            "qualifier index" << endl;
          return;
        }

        orig_decl = current_scope ();
        decl = declaration (orig_decl);
      }
      else
      {
        // This is a specifier. The saved tokens sequence contains all the
        // tokens in this pragma until EOF.
        //
        cxx_tokens_lexer l;
        l.start (saved_tokens, loc);
        handle_pragma (
          l, db, "index", "member", val, declaration (), "", false);
        return;
      }
    }

    relational::index in;
    in.loc = loc;

    if (tt == CPP_OPEN_PAREN)
    {
      // Specifier with the class fq-name, index name, or both.
      //
      // index(<fq-name>)
      // index("<name>")
      // index(<fq-name>::"<name>")
      //
      tt = l.next (tl, &tn);

      // Resolve class name, if any.
      //
      if (tt == CPP_NAME || tt == CPP_SCOPE)
      {
        cpp_ttype ptt;
        orig_decl = resolve_scoped_name (
          l, tt, tl, tn, current_scope (), decl_name, true, p, true, &ptt);

        if (orig_decl == 0)
          return; // Diagnostics has already been issued.

        // Get the actual type if this is a TYPE_DECL. Also get the main
        // variant.
        //
        if (TREE_CODE (orig_decl) == TYPE_DECL)
          orig_decl = TREE_TYPE (orig_decl);

        if (TYPE_P (orig_decl)) // Can be a template.
          decl = declaration (TYPE_MAIN_VARIANT (orig_decl));
        else
          decl = declaration (orig_decl);

        if (tt == CPP_STRING && ptt != CPP_SCOPE)
        {
          error (l) << "'::' expected before index name in db pragma " << p
                    << endl;
          return;
        }

        if (tt != CPP_STRING && ptt == CPP_SCOPE)
        {
          error (l) << "index name expected after '::' in db pragma " << p
                    << endl;
          return;
        }
      }
      else
      {
        orig_decl = current_scope ();
        decl = declaration (orig_decl);
      }

      // Make sure we've got the correct declaration type.
      //
      if (!check_qual_decl_type (decl, decl_name, p, loc))
        return;

      if (tt == CPP_STRING)
      {
        in.name = tl;
        tt = l.next (tl, &tn);
      }

      if (tt != CPP_CLOSE_PAREN)
      {
        error (l) << "')' expected at the end of db pragma " << p << endl;
        return;
      }

      tt = l.next (tl, &tn);
    }

    val = in;
    adder = &accumulate<relational::index>;
  }
  else if (p == "namespace")
  {
    // namespace [(<identifier>)]
    // namespace ()                (refers to global namespace)
    //

    tt = l.next (tl, &tn);

    if (tt == CPP_OPEN_PAREN)
    {
      tt = l.next (tl, &tn);

      if (tt == CPP_NAME || tt == CPP_SCOPE)
      {
        orig_decl = resolve_scoped_name (
          l, tt, tl, tn, current_scope (), decl_name, false, p);

        if (orig_decl == 0)
          return; // Diagnostics has already been issued.

        // Make sure we've got the correct declaration type.
        //
        if (!check_qual_decl_type (orig_decl, decl_name, p, loc))
          return;

        // Resolve namespace aliases if any.
        //
        orig_decl = ORIGINAL_NAMESPACE (orig_decl);
        decl = declaration (orig_decl);

        if (tt != CPP_CLOSE_PAREN)
        {
          error (l) << "')' expected at the end of db pragma " << p << endl;
          return;
        }

        tt = l.next (tl, &tn);
      }
      else if (tt == CPP_CLOSE_PAREN)
      {
        orig_decl = global_namespace;
        decl = declaration (orig_decl);
        tt = l.next (tl, &tn);
      }
      else
      {
        error (l) << "data member name expected in db pragma " << p << endl;
        return;
      }
    }
    else
    {
      // Make sure we are in a namespace scope.
      //
      if (TREE_CODE (current_scope ()) != NAMESPACE_DECL)
      {
        error (l) << "db pragma " << p << " is not in a namespace scope"
                  << endl;
        return;
      }

      ns = true;
    }
  }
  else if (p == "object" ||
           p == "view" ||
           p == "value")
  {
    // object [(<identifier>)]
    // view [(<identifier>)]
    // value [(<identifier>)]
    //

    tt = l.next (tl, &tn);

    if (tt == CPP_OPEN_PAREN)
    {
      tt = l.next (tl, &tn);

      // Can be built-in type (e.g., bool).
      //
      if (tt == CPP_NAME || tt == CPP_KEYWORD || tt == CPP_SCOPE)
      {
        orig_decl = resolve_scoped_name (
          l, tt, tl, tn, current_scope (), decl_name, true, p);

        if (orig_decl == 0)
          return; // Diagnostics has already been issued.

        // Get the actual type if this is a TYPE_DECL. Also get the main
        // variant.
        //
        if (TREE_CODE (orig_decl) == TYPE_DECL)
          orig_decl = TREE_TYPE (orig_decl);

        if (TYPE_P (orig_decl)) // Can be a template.
          decl = declaration (TYPE_MAIN_VARIANT (orig_decl));
        else
          decl = declaration (orig_decl);

        // Make sure we've got the correct declaration type.
        //
        if (!check_qual_decl_type (decl, decl_name, p, loc))
          return;

        if (tt != CPP_CLOSE_PAREN)
        {
          error (l) << "')' expected at the end of db pragma " << p << endl;
          return;
        }

        tt = l.next (tl, &tn);
      }
      else
      {
        error (l) << "type name expected in db pragma " << p << endl;
        return;
      }
    }
  }
  else if (p == "member")
  {
    // member [(<identifier>)]
    //

    tt = l.next (tl, &tn);

    if (tt == CPP_OPEN_PAREN)
    {
      tt = l.next (tl, &tn);

      if (tt != CPP_NAME && tt != CPP_SCOPE)
      {
        error (l) << "data member name expected in db pragma " << p << endl;
        return;
      }

      // We need to see if this is a virtual data member declaration. Also,
      // if it is not, then the name can still refer to one so we need to
      // take extra steps to handle that. But first, we save the name and
      // look for the 'virtual' specifier.
      //
      cxx_tokens name_tokens;
      for (; tt != CPP_CLOSE_PAREN && tt != CPP_EOF; tt = l.next (tl, &tn))
        name_tokens.push_back (cxx_token (l.location (), tt, tl, tn));

      if (tt != CPP_CLOSE_PAREN)
      {
        error (l) << "')' expected at the end of db pragma " << p << endl;
        return;
      }

      // Now scan the remainder of the pragma looking for the 'virtual'
      // keyword and saving the tokens in between for later.
      //
      bool virt (false);
      size_t balance (0);
      for (tt = l.next (tl, &tn); tt != CPP_EOF; tt = l.next (tl, &tn))
      {
        switch (tt)
        {
        case CPP_OPEN_PAREN:
          {
            balance++;
            break;
          }
        case CPP_CLOSE_PAREN:
          {
            if (balance > 0)
              balance--;
            else
            {
              error (l) << "unbalanced parenthesis in db pragma " << p << endl;
              return;
            }
            break;
          }
        default:
          {
            if (balance == 0 && tt == CPP_KEYWORD && tl == "virtual")
              virt = true;
            break;
          }
        }

        if (virt)
          break;

        saved_tokens.push_back (cxx_token (l.location (), tt, tl, tn));
      }

      if (balance != 0)
      {
        error (l) << "unbalanced parenthesis in db pragma " << p << endl;
        return;
      }

      // Regardless of whether this is a virtual member declaration or a
      // reference, resolve its scope name (if one is specified), which
      // should be a class. We will need it in both cases.
      //
      tree scope;
      if (name_tokens.size () > 2) // scope::name
      {
        size_t n (name_tokens.size ());

        if (name_tokens[n - 2].type != CPP_SCOPE ||
            name_tokens[n - 1].type != CPP_NAME)
        {
          error (l) << "invalid name in db pragma " << p << endl;
          return;
        }

        cxx_tokens scope_tokens (1, name_tokens.back ());
        name_tokens.pop_back (); // ::
        name_tokens.pop_back (); // name
        name_tokens.swap (scope_tokens);

        cxx_tokens_lexer l;
        l.start (scope_tokens);

        tree tn;
        string tl;
        cpp_ttype tt (l.next (tl));

        scope = resolve_scoped_name (
          l, tt, tl, tn, current_scope (), decl_name, true, p);

        if (scope == 0)
          return; // Diagnostics has already been issued.

        scope = TYPE_MAIN_VARIANT (TREE_TYPE (scope));

        if (tt != CPP_EOF)
        {
          error (l) << "invalid name in db pragma " << p << endl;
          return;
        }

        decl_name += "::";
      }
      else
        scope = current_scope ();

      if (virt)
      {
        // Should be a single name.
        //
        if (name_tokens.size () > 1 || name_tokens.back ().type != CPP_NAME)
        {
          location_t l (name_tokens.back ().loc);
          error (l) << "invalid name in db pragma " << p << endl;
          return;
        }
        string const& name (name_tokens.back ().literal);

        // Parse the remainder of the virtual specifier.
        //
        // virtual(<fq-name>)
        //
        tree type;
        string type_name;
        location_t ord (loc);
        int ord_bias (0);
        {
          string p (tl);
          location_t loc (l.location ());

          if (l.next (tl, &tn) != CPP_OPEN_PAREN)
          {
            error (l) << "'(' expected after db pragma " << p << endl;
            return;
          }

          tt = l.next (tl, &tn);

          // Can be built-in type (e.g., bool).
          //
          if (tt == CPP_NAME || tt == CPP_KEYWORD || tt == CPP_SCOPE)
          {
            type = resolve_scoped_name (
              l, tt, tl, tn, current_scope (), type_name, true, p);

            if (type == 0)
              return; // Diagnostics has already been issued.

            if (TREE_CODE (type) != TYPE_DECL)
            {
              error (loc) << "name '" << type_name << "' in db pragma "
                          << p << " does not refer to a type" << endl;
              return;
            }

            type = TREE_TYPE (type);
          }
          else
          {
            error (l) << "type name expected in db pragma " << p << endl;
            return;
          }

          if (tt != CPP_CLOSE_PAREN)
          {
            error (l) << "')' expected at the end of db pragma " << p << endl;
            return;
          }

          tt = l.next (tl, &tn);

          // See if we have before/after specifiers.
          //
          if (tt == CPP_NAME && tl == "before")
          {
            // before[(<member>)]
            //
            // Before without the member name means first.
            //
            tt = l.next (tl, &tn);

            if (tt == CPP_OPEN_PAREN)
            {
              if (l.next (tl, &tn) != CPP_NAME)
              {
                error (l) << "')' member name expected in db pragma before"
                          << endl;
              }

              string dn;
              cxx_tokens ts (1, cxx_token (l.location (), CPP_NAME, tl));
              declaration d (resolve_data_member (scope, ts, dn, "before"));

              if (!d)
                return; // Diagnostics has already been issued.

              if (d.virt)
              {
                ord = d.decl.virt->ord;
                ord_bias = d.decl.virt->ord_bias - 1;
              }
              else
              {
                ord = real_source_location (d.decl.real);
                ord_bias = -1;
              }

              if (l.next (tl, &tn) != CPP_CLOSE_PAREN)
              {
                error (l) << "')' expected at the end of db pragma before"
                          << endl;
                return;
              }

              tt = l.next (tl, &tn);
            }
            else
              ord = 0;
          }

          if (tt == CPP_NAME && tl == "after")
          {
            // after[(<member>)]
            //
            // Before without the member name means last.
            //
            tt = l.next (tl, &tn);

            if (tt == CPP_OPEN_PAREN)
            {
              if (l.next (tl, &tn) != CPP_NAME)
              {
                error (l) << "')' member name expected in db pragma after"
                          << endl;
              }

              string dn;
              cxx_tokens ts (1, cxx_token (l.location (), CPP_NAME, tl));
              declaration d (resolve_data_member (scope, ts, dn, "after"));

              if (!d)
                return; // Diagnostics has already been issued.

              if (d.virt)
              {
                ord = d.decl.virt->ord;
                ord_bias = d.decl.virt->ord_bias + 1;
              }
              else
              {
                ord = real_source_location (d.decl.real);
                ord_bias = 1;
              }

              if (l.next (tl, &tn) != CPP_CLOSE_PAREN)
              {
                error (l) << "')' expected at the end of db pragma after"
                          << endl;
                return;
              }

              tt = l.next (tl, &tn);
            }
            else
              ord = ~location_t (0);
          }
        }

        pair<virt_declaration_set::const_iterator, bool> r (
          virt_declarations_[scope].insert (
            virt_declaration (loc, ord, ord_bias, name, FIELD_DECL, type)));

        if (!r.second)
        {
          error (loc) << "virtual data member declaration '" << name
                      << "' conflicts with a previous declaration" << endl;

          info (r.first->loc) << "'" << name << "' was previously "
                              << "declared here" << endl;
          return;
        }

        decl_name += name;
        decl = declaration (*r.first);

        // Mark it as virtual using the standard pragma machinery.
        //
        add_pragma (
          pragma ("virtual", "virtual", true, loc, &check_spec_decl_type, 0),
          decl,
          false);
      }
      else
      {
        // Not a virtual member declaration.
        //
        decl = resolve_data_member (scope, name_tokens, decl_name, p);

        if (!decl)
          return; // Diagnostics has already been issued.

        // Make sure we've got the correct declaration type.
        //
        if (!check_qual_decl_type (decl, decl_name, p, loc))
          return;
      }
    }
  }
  //
  // The member qualifier can be omitted so we need to also handle all
  // the member pragmas here.
  //
  else if (p == "id" ||
           p == "auto" ||
           p == "unique" ||
           p == "get" ||
           p == "set" ||
           p == "access" ||
           p == "column" ||
           p == "value_column" ||
           p == "index_column" ||
           p == "key_column" ||
           p == "id_column" ||
           p == "options" ||
           p == "value_options" ||
           p == "index_options" ||
           p == "key_options" ||
           p == "id_options" ||
           p == "type" ||
           p == "id_type" ||
           p == "value_type" ||
           p == "index_type" ||
           p == "key_type" ||
           p == "table" ||
           p == "null" ||
           p == "not_null" ||
           p == "key_null" ||
           p == "key_not_null" ||
           p == "value_null" ||
           p == "value_not_null" ||
           p == "default" ||
           p == "section" ||
           p == "load"    ||
           p == "update"  ||
           p == "inverse" ||
           p == "on_delete" ||
           p == "points_to" ||
           p == "unordered" ||
           p == "readonly" ||
           p == "transient" ||
           p == "added" ||
           p == "deleted" ||
           p == "version" ||
           p == "virtual" ||
           p == "direct_load" ||
           p == "indirect_load")
  {
    handle_pragma (l, db, p, "member", val, declaration (), "", false);
    return;
  }
  else
  {
    error (l) << "unknown db pragma " << p << endl;
    return;
  }

  // Record this pragma. Delay this until after we process the
  // specifiers for value (see comment below for the reason).
  //
  if (adder == 0)
    val = orig_decl;

  pragma prag (p,
               name, // For now no need to translate '_' to '-'.
               val,
               loc,
               &check_qual_decl_type,
               adder != 0 ? adder : &add_qual_entry);

  tree scope;
  if (!decl)
  {
    scope = current_scope ();

    if (!ns && !CLASS_TYPE_P (scope))
      scope = global_namespace;
  }

  any* pval;
  if (p != "value")
  {
    if (decl)
      pval = &decl_pragmas_[decl].insert (prag).value;
    else
    {
      if (!ns)
      {
        pragma_list& pl (loc_pragmas_[scope]);
        pl.push_back (prag);
        pval = &pl.back ().value;
      }
      else
      {
        ns_loc_pragmas_.push_back (ns_loc_pragma (scope, prag));
        pval = &ns_loc_pragmas_.back ().pragma.value;
      }
    }
  }
  else
    pval = &val;

  // See if there are any saved tokens to replay.
  //
  if (!saved_tokens.empty ())
  {
    cxx_tokens_lexer l;
    l.start (saved_tokens);

    string tl;
    cpp_ttype tt (l.next (tl));

    if (tt == CPP_NAME || tt == CPP_KEYWORD)
    {
      handle_pragma (l, "", tl, p, *pval, decl, decl_name, ns);

      if (errorcount != 0) // Avoid parsing the rest if there was an error.
        return;
    }
    else if (tt != CPP_EOF)
    {
      error (l) << "unexpected text after " << p << " in db pragma" << endl;
      return;
    }
  }

  size_t count (0);
  if (tt == CPP_NAME || tt == CPP_KEYWORD)
  {
    if (decl)
      count = decl_pragmas_[decl].size ();
    else
      count = loc_pragmas_[scope].size ();

    handle_pragma (l, "", tl, p, *pval, decl, decl_name, ns);
  }
  else if (tt != CPP_EOF)
  {
    error (l) << "unexpected text after " << p << " in db pragma" << endl;
    return;
  }

  // Record the value pragma. Here things are complicated by the fact
  // that we use the value pragma by itself to signify a composite value
  // type declaration. Consider this example:
  //
  // #pragma db value pgsql:type("POINT")
  // class point {...};
  //
  // Should this class be considered composite value type in other
  // databases (because that's what would happen by default)? Probably
  // not. So to overcome this we are going to detect and ignore cases
  // where (a) some specifiers followed the value qualifier but (b)
  // none of them are for the database that we are compiling for.
  //
  if (p == "value")
  {
    if (decl)
    {
      pragma_set& ps (decl_pragmas_[decl]);

      if (tt == CPP_EOF || ps.size () > count)
        ps.insert (prag);
    }
    else
    {
      pragma_list& pl (loc_pragmas_[scope]);

      if (tt == CPP_EOF || pl.size () > count)
        pl.push_back (prag);
    }
  }
}

/*
extern "C" void
handle_pragma_db_mysql (cpp_reader* r)
{
  handle_pragma_qualifier (r, "mysql");
}

extern "C" void
handle_pragma_db_sqlite (cpp_reader* r)
{
  handle_pragma_qualifier (r, "sqlite");
}

extern "C" void
handle_pragma_db_pgsql (cpp_reader* r)
{
  handle_pragma_qualifier (r, "pgsql");
}

extern "C" void
handle_pragma_db_oracle (cpp_reader* r)
{
  handle_pragma_qualifier (r, "oracle");
}

extern "C" void
handle_pragma_db_mssql (cpp_reader* r)
{
  handle_pragma_qualifier (r, "mssql");
}

extern "C" void
handle_pragma_db_model (cpp_reader* r)
{
  handle_pragma_qualifier (r, "model");
}

extern "C" void
handle_pragma_db_map (cpp_reader* r)
{
  handle_pragma_qualifier (r, "map");
}

extern "C" void
handle_pragma_db_namespace (cpp_reader* r)
{
  handle_pragma_qualifier (r, "namespace");
}

extern "C" void
handle_pragma_db_object (cpp_reader* r)
{
  handle_pragma_qualifier (r, "object");
}

extern "C" void
handle_pragma_db_view (cpp_reader* r)
{
  handle_pragma_qualifier (r, "view");
}

extern "C" void
handle_pragma_db_value (cpp_reader* r)
{
  handle_pragma_qualifier (r, "value");
}

extern "C" void
handle_pragma_db_member (cpp_reader* r)
{
  handle_pragma_qualifier (r, "member");
}

extern "C" void
handle_pragma_db_id (cpp_reader* r)
{
  handle_pragma_qualifier (r, "id");
}

extern "C" void
handle_pragma_db_auto (cpp_reader* r)
{
  handle_pragma_qualifier (r, "auto");
}

extern "C" void
handle_pragma_db_column (cpp_reader* r)
{
  handle_pragma_qualifier (r, "column");
}

extern "C" void
handle_pragma_db_vcolumn (cpp_reader* r)
{
  handle_pragma_qualifier (r, "value_column");
}

extern "C" void
handle_pragma_db_icolumn (cpp_reader* r)
{
  handle_pragma_qualifier (r, "index_column");
}

extern "C" void
handle_pragma_db_kcolumn (cpp_reader* r)
{
  handle_pragma_qualifier (r, "key_column");
}

extern "C" void
handle_pragma_db_idcolumn (cpp_reader* r)
{
  handle_pragma_qualifier (r, "id_column");
}

extern "C" void
handle_pragma_db_options (cpp_reader* r)
{
  handle_pragma_qualifier (r, "options");
}

extern "C" void
handle_pragma_db_voptions (cpp_reader* r)
{
  handle_pragma_qualifier (r, "value_options");
}

extern "C" void
handle_pragma_db_ioptions (cpp_reader* r)
{
  handle_pragma_qualifier (r, "index_options");
}

extern "C" void
handle_pragma_db_koptions (cpp_reader* r)
{
  handle_pragma_qualifier (r, "key_options");
}

extern "C" void
handle_pragma_db_idoptions (cpp_reader* r)
{
  handle_pragma_qualifier (r, "id_options");
}

extern "C" void
handle_pragma_db_type (cpp_reader* r)
{
  handle_pragma_qualifier (r, "type");
}

extern "C" void
handle_pragma_db_id_type (cpp_reader* r)
{
  handle_pragma_qualifier (r, "id_type");
}

extern "C" void
handle_pragma_db_vtype (cpp_reader* r)
{
  handle_pragma_qualifier (r, "value_type");
}

extern "C" void
handle_pragma_db_itype (cpp_reader* r)
{
  handle_pragma_qualifier (r, "index_type");
}

extern "C" void
handle_pragma_db_ktype (cpp_reader* r)
{
  handle_pragma_qualifier (r, "key_type");
}

extern "C" void
handle_pragma_db_table (cpp_reader* r)
{
  handle_pragma_qualifier (r, "table");
}

extern "C" void
handle_pragma_db_null (cpp_reader* r)
{
  handle_pragma_qualifier (r, "null");
}

extern "C" void
handle_pragma_db_not_null (cpp_reader* r)
{
  handle_pragma_qualifier (r, "not_null");
}

extern "C" void
handle_pragma_db_value_null (cpp_reader* r)
{
  handle_pragma_qualifier (r, "value_null");
}

extern "C" void
handle_pragma_db_value_not_null (cpp_reader* r)
{
  handle_pragma_qualifier (r, "value_not_null");
}

extern "C" void
handle_pragma_db_key_null (cpp_reader* r)
{
  handle_pragma_qualifier (r, "key_null");
}

extern "C" void
handle_pragma_db_key_not_null (cpp_reader* r)
{
  handle_pragma_qualifier (r, "key_not_null");
}

extern "C" void
handle_pragma_db_default (cpp_reader* r)
{
  handle_pragma_qualifier (r, "default");
}

extern "C" void
handle_pragma_db_section (cpp_reader* r)
{
  handle_pragma_qualifier (r, "section");
}

extern "C" void
handle_pragma_db_load (cpp_reader* r)
{
  handle_pragma_qualifier (r, "load");
}

extern "C" void
handle_pragma_db_update (cpp_reader* r)
{
  handle_pragma_qualifier (r, "update");
}

extern "C" void
handle_pragma_db_inverse (cpp_reader* r)
{
  handle_pragma_qualifier (r, "inverse");
}

extern "C" void
handle_pragma_db_on_delete (cpp_reader* r)
{
  handle_pragma_qualifier (r, "on_delete");
}

extern "C" void
handle_pragma_db_points_to (cpp_reader* r)
{
  handle_pragma_qualifier (r, "points_to");
}

extern "C" void
handle_pragma_db_unordered (cpp_reader* r)
{
  handle_pragma_qualifier (r, "unordered");
}

extern "C" void
handle_pragma_db_readonly (cpp_reader* r)
{
  handle_pragma_qualifier (r, "readonly");
}

extern "C" void
handle_pragma_db_transient (cpp_reader* r)
{
  handle_pragma_qualifier (r, "transient");
}

extern "C" void
handle_pragma_db_added (cpp_reader* r)
{
  handle_pragma_qualifier (r, "added");
}

extern "C" void
handle_pragma_db_deleted (cpp_reader* r)
{
  handle_pragma_qualifier (r, "deleted");
}

extern "C" void
handle_pragma_db_version (cpp_reader* r)
{
  handle_pragma_qualifier (r, "version");
}

extern "C" void
handle_pragma_db_virtual (cpp_reader* r)
{
  handle_pragma_qualifier (r, "virtual");
}
*/

static void
handle_pragma_db (cxx_lexer& l)
{
  string tl;
  cpp_ttype tt (l.next (tl));

  if (tt != CPP_NAME && tt != CPP_KEYWORD)
  {
    error (l) << "expected specifier after db pragma" << endl;
    return;
  }

  handle_pragma_qualifier (l, tl);
}

namespace
{
  struct deferred_pragma
  {
    location_t loc;
    cxx_tokens tokens;
  };

  vector<deferred_pragma> deferred_pragmas_;

  // Convert a numeric literal token into an INTEGER_CST/REAL_CST node
  // similar to what GCC's pragma lexer does (the type is determined
  // according to the C++ rules). Return the null tree on error.
  //
  tree
  make_number (clang::Preprocessor& pp, clang::Token const& t)
  {
    llvm::SmallString<64> buf;
    bool invalid (false);
    llvm::StringRef spelling (pp.getSpelling (t, buf, &invalid));

    if (invalid)
      return tree ();

    clang::NumericLiteralParser lp (spelling,
                                    t.getLocation (),
                                    pp.getSourceManager (),
                                    pp.getLangOpts (),
                                    pp.getTargetInfo (),
                                    pp.getDiagnostics ());
    if (lp.hadError)
      return tree ();

    if (lp.isIntegerLiteral ())
    {
      llvm::APInt v (64, 0);

      if (lp.GetIntegerValue (v))
        return tree (); // Overflow.

      unsigned long long uv (v.getZExtValue ());

      clang::TargetInfo const& ti (pp.getTargetInfo ());
      unsigned iw (ti.getIntWidth ()), lw (ti.getLongWidth ());

      unsigned long long int_max ((1ULL << (iw - 1)) - 1);
      unsigned long long long_max ((1ULL << (lw - 1)) - 1);
      unsigned long long uint_max (iw >= 64 ? ~0ULL : (1ULL << iw) - 1);
      unsigned long long ulong_max (lw >= 64 ? ~0ULL : (1ULL << lw) - 1);
      unsigned long long llong_max (0x7FFFFFFFFFFFFFFFULL);

      bool dec (lp.getRadix () == 10);
      bool u (lp.isUnsigned);
      cst_type ct;

      if (!lp.isLong && !lp.isLongLong && !u && uv <= int_max)
        ct = cst_int;
      else if (!lp.isLong && !lp.isLongLong && (u || !dec) && uv <= uint_max)
        ct = cst_unsigned;
      else if (!lp.isLongLong && !u && uv <= long_max)
        ct = cst_long;
      else if (!lp.isLongLong && (u || !dec) && uv <= ulong_max)
        ct = cst_unsigned_long;
      else if (!u && uv <= llong_max)
        ct = cst_long_long;
      else
        ct = cst_unsigned_long_long;

      return make_int_cst (ct, uv);
    }
    else if (lp.isFloatingLiteral ())
    {
      llvm::APFloat v (llvm::APFloat::IEEEdouble ());
      lp.GetFloatValue (v, llvm::RoundingMode::NearestTiesToEven);

      cst_type ct (lp.isFloat
                   ? cst_float
                   : lp.isLong ? cst_long_double : cst_double);

      return make_real_cst (ct, v.convertToDouble (), spelling.str ());
    }

    return tree ();
  }

  struct db_pragma_handler: clang::PragmaHandler
  {
    db_pragma_handler (): clang::PragmaHandler ("db") {}

    virtual void
    HandlePragma (clang::Preprocessor& pp,
                  clang::PragmaIntroducer intro,
                  clang::Token&)
    {
      clang::SourceManager& sm (pp.getSourceManager ());

      deferred_pragmas_.push_back (deferred_pragma ());
      deferred_pragma& p (deferred_pragmas_.back ());
      p.loc = location_t (
        sm.getExpansionLoc (intro.Loc).getRawEncoding ());

      // Collect the (macro-expanded) pragma tokens.
      //
      for (;;)
      {
        clang::Token t;
        pp.Lex (t);

        clang::tok::TokenKind k (t.getKind ());

        if (k == clang::tok::eod || k == clang::tok::eof)
          break;

        location_t loc (
          sm.getExpansionLoc (t.getLocation ()).getRawEncoding ());

        cpp_ttype tt (odbc::token_type (k));
        string lit;
        tree node;

        switch (tt)
        {
        case CPP_NAME:
        case CPP_KEYWORD:
          {
            if (clang::IdentifierInfo* ii = t.getIdentifierInfo ())
              lit = ii->getName ().str ();
            else
              lit = pp.getSpelling (t);

            tt = odbc::keyword_p (lit) ? CPP_KEYWORD : CPP_NAME;
            break;
          }
        case CPP_STRING:
        case CPP_WSTRING:
        case CPP_STRING16:
        case CPP_STRING32:
        case CPP_UTF8STRING:
          {
            // Like GCC's pragma lexer, join adjacent string literals and
            // return the string value without the quotes and with escape
            // sequences processed.
            //
            llvm::SmallVector<clang::Token, 4> ts (1, t);

            while (clang::tok::isStringLiteral (pp.LookAhead (0).getKind ()))
            {
              clang::Token n;
              pp.Lex (n);
              ts.push_back (n);
            }

            clang::StringLiteralParser lp (ts, pp);
            if (lp.hadError)
              lit = pp.getSpelling (t);
            else
              lit = lp.GetString ().str ();

            tt = CPP_STRING;
            break;
          }
        case CPP_NUMBER:
          {
            node = make_number (pp, t);
            lit = pp.getSpelling (t);

            if (!node)
              node = error_mark_node;
            break;
          }
        case N_TTYPES:
          {
            tt = CPP_OTHER;
            lit = pp.getSpelling (t);
            break;
          }
        default:
          {
            lit = pp.getSpelling (t);
            break;
          }
        }

        p.tokens.push_back (cxx_token (loc, tt, lit, node));
      }
    }
  };

  // Return true if the declaration's source range contains the location.
  //
  bool
  contains (clang::Decl const* d, clang::SourceLocation l)
  {
    clang::SourceManager& sm (odbc::sm ());
    clang::SourceRange r (d->getSourceRange ());

    if (r.isInvalid ())
      return false;

    clang::SourceLocation b (sm.getExpansionLoc (r.getBegin ()));
    clang::SourceLocation e (sm.getExpansionLoc (r.getEnd ()));

    return !sm.isBeforeInTranslationUnit (l, b) &&
      !sm.isBeforeInTranslationUnit (e, l);
  }

  // Find the innermost namespace or class that lexically contains the
  // location. This is the equivalent of GCC's current_scope() at the
  // point where the pragma was encountered.
  //
  clang::DeclContext*
  scope_at (clang::DeclContext* dc, clang::SourceLocation l)
  {
    using namespace clang;

    for (;;)
    {
      DeclContext* n (0);

      for (Decl* d: dc->decls ())
      {
        DeclContext* c (0);

        if (NamespaceDecl* ns = dyn_cast<NamespaceDecl> (d))
          c = ns;
        else if (LinkageSpecDecl* ls = dyn_cast<LinkageSpecDecl> (d))
          c = ls;
        else if (ExportDecl* ed = dyn_cast<ExportDecl> (d))
          c = ed;
        else if (CXXRecordDecl* r = dyn_cast<CXXRecordDecl> (d))
        {
          if (ClassTemplateSpecializationDecl* s =
                dyn_cast<ClassTemplateSpecializationDecl> (r))
          {
            if (s->getSpecializationKind () == TSK_ImplicitInstantiation)
              continue;
          }

          if (r->isThisDeclarationADefinition ())
            c = r;
        }
        else if (ClassTemplateDecl* t = dyn_cast<ClassTemplateDecl> (d))
        {
          CXXRecordDecl* r (t->getTemplatedDecl ());

          if (r->isThisDeclarationADefinition ())
            c = r;
        }

        if (c != 0 && contains (d, l))
        {
          n = c;
          break;
        }
      }

      if (n == 0)
        return dc;

      dc = n;
    }
  }
}

void
register_odb_pragmas (clang::Preprocessor& pp)
{
  // Note that similar to the GCC plugin we register 'db' as a pragma
  // rather than a namespace so that the specifier or qualifier name is
  // macro-expanded.
  //
  pp.AddPragmaHandler (new db_pragma_handler);
}

void
process_odb_pragmas ()
{
  clang::TranslationUnitDecl* tu (odbc::ast ().getTranslationUnitDecl ());

  for (deferred_pragma const& p: deferred_pragmas_)
  {
    clang::SourceLocation l (odbc::source_location (p.loc));
    current_scope_ = odbc::make_scope (scope_at (tu, l));

    cxx_tokens_lexer lex;
    lex.start (p.tokens, p.loc);
    handle_pragma_db (lex);
  }

  current_scope_ = tree ();
  deferred_pragmas_.clear ();
}

void
post_process_pragmas ()
{
  // Make sure object, view, and composite class template instantiations
  // are fully instantiated.
  //
  for (decl_pragmas::iterator i (decl_pragmas_.begin ()),
         e (decl_pragmas_.end ()); i != e; ++i)
  {
    if (i->first.virt)
      continue;

    tree type (i->first.decl.real);

    if (!(CLASS_TYPE_P (type) && CLASSTYPE_TEMPLATE_INSTANTIATION (type)))
      continue;

    // Check whether this is an object, view, or composite value type.
    //
    pragma const* p (0);
    string diag_name;

    for (pragma_set::iterator j (i->second.begin ()), e (i->second.end ());
         j != e; ++j)
    {
      string const& name (j->second.context_name);

      if (name == "object")
      {
        p = &j->second;
        diag_name = "persistent object";
        break;
      }
      else if (name == "view")
      {
        p = &j->second;
        diag_name = "view";
        break;
      }
      else if (name == "value")
      {
        p = &j->second;
        diag_name = "composite value";
        break;
      }
      // We don't want to instantiate simple values since they may be
      // incomplete.
      //
      else if (name == "simple" || name == "container")
      {
        p = 0;
        break;
      }
    }

    if (p == 0)
      continue;

    // Make sure it is instantiated.
    //
    tree decl (TYPE_NAME (p->value.value<tree> ()));
    location_t loc (real_source_location (decl));

    if (instantiate_class_template (type, loc) == error_mark_node ||
        errorcount != 0 ||
        !COMPLETE_TYPE_P (type))
    {
      error (loc) << "unable to instantiate " << diag_name << " class template"
                  << endl;
      throw pragmas_failed ();
    }
  }
}

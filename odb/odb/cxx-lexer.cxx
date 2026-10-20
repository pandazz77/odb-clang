// file      : odb/cxx-lexer.cxx
// license   : GNU GPL v3; see accompanying LICENSE file

#include <odb/clang.hxx>

#include <cassert>
#include <iostream>

#include <clang/Basic/IdentifierTable.h>
#include <clang/Basic/LangOptions.h>
#include <clang/Lex/Lexer.h>

#include <odb/cxx-lexer.hxx>

using namespace std;

//
// cxx_lexer
//

// Token spelling. See cpplib.h for details.
//
#define OP(e, s) s ,
#define TK(e, s) #e ,
char const* cxx_lexer::token_spelling[N_TTYPES + 1] = { TTYPE_TABLE "KEYWORD"};
#undef OP
#undef TK

cxx_lexer::
~cxx_lexer ()
{
}

namespace odbc
{
  cpp_ttype
  token_type (clang::tok::TokenKind k)
  {
    using namespace clang::tok;

    switch (k)
    {
    case clang::tok::equal:                return CPP_EQ;
    case exclaim:              return CPP_NOT;
    case clang::tok::greater:              return CPP_GREATER;
    case clang::tok::less:                 return CPP_LESS;
    case clang::tok::plus:                 return CPP_PLUS;
    case clang::tok::minus:                return CPP_MINUS;
    case star:                 return CPP_MULT;
    case slash:                return CPP_DIV;
    case percent:              return CPP_MOD;
    case amp:                  return CPP_AND;
    case pipe:                 return CPP_OR;
    case caret:                return CPP_XOR;
    case greatergreater:       return CPP_RSHIFT;
    case lessless:             return CPP_LSHIFT;
    case tilde:                return CPP_COMPL;
    case ampamp:               return CPP_AND_AND;
    case pipepipe:             return CPP_OR_OR;
    case question:             return CPP_QUERY;
    case colon:                return CPP_COLON;
    case comma:                return CPP_COMMA;
    case l_paren:              return CPP_OPEN_PAREN;
    case r_paren:              return CPP_CLOSE_PAREN;
    case clang::tok::eof:
    case eod:                  return CPP_EOF;
    case equalequal:           return CPP_EQ_EQ;
    case exclaimequal:         return CPP_NOT_EQ;
    case greaterequal:         return CPP_GREATER_EQ;
    case lessequal:            return CPP_LESS_EQ;
    case spaceship:            return CPP_SPACESHIP;
    case plusequal:            return CPP_PLUS_EQ;
    case minusequal:           return CPP_MINUS_EQ;
    case starequal:            return CPP_MULT_EQ;
    case slashequal:           return CPP_DIV_EQ;
    case percentequal:         return CPP_MOD_EQ;
    case ampequal:             return CPP_AND_EQ;
    case pipeequal:            return CPP_OR_EQ;
    case caretequal:           return CPP_XOR_EQ;
    case greatergreaterequal:  return CPP_RSHIFT_EQ;
    case lesslessequal:        return CPP_LSHIFT_EQ;
    case clang::tok::hash:                 return CPP_HASH;
    case hashhash:             return CPP_PASTE;
    case l_square:             return CPP_OPEN_SQUARE;
    case r_square:             return CPP_CLOSE_SQUARE;
    case l_brace:              return CPP_OPEN_BRACE;
    case r_brace:              return CPP_CLOSE_BRACE;
    case semi:                 return CPP_SEMICOLON;
    case ellipsis:             return CPP_ELLIPSIS;
    case plusplus:             return CPP_PLUS_PLUS;
    case minusminus:           return CPP_MINUS_MINUS;
    case arrow:                return CPP_DEREF;
    case period:               return CPP_DOT;
    case coloncolon:           return CPP_SCOPE;
    case arrowstar:            return CPP_DEREF_STAR;
    case periodstar:           return CPP_DOT_STAR;
    case at:                   return CPP_ATSIGN;
    case identifier:
    case raw_identifier:       return CPP_NAME;
    case numeric_constant:     return CPP_NUMBER;
    case char_constant:        return CPP_CHAR;
    case wide_char_constant:   return CPP_WCHAR;
    case utf16_char_constant:  return CPP_CHAR16;
    case utf32_char_constant:  return CPP_CHAR32;
    case utf8_char_constant:   return CPP_UTF8CHAR;
    case string_literal:       return CPP_STRING;
    case wide_string_literal:  return CPP_WSTRING;
    case utf8_string_literal:  return CPP_UTF8STRING;
    case utf16_string_literal: return CPP_STRING16;
    case utf32_string_literal: return CPP_STRING32;
    case header_name:          return CPP_HEADER_NAME;
    case comment:              return CPP_COMMENT;
    case unknown:              return CPP_OTHER;
    default:
      break;
    }

    if (getKeywordSpelling (k) != 0)
      return CPP_KEYWORD;

    return N_TTYPES;
  }

  static clang::LangOptions
  default_lang_options ()
  {
    clang::LangOptions lo;
    lo.CPlusPlus = lo.CPlusPlus11 = lo.CPlusPlus14 = lo.CPlusPlus17 = 1;
    lo.Bool = 1;
    lo.WChar = 1;
    lo.CXXOperatorNames = 1;
    lo.LineComment = 1;
    lo.Digraphs = 1;
    return lo;
  }

  static clang::LangOptions const&
  lang_options ()
  {
    if (ast_context != 0)
      return ast_context->getLangOpts ();

    static clang::LangOptions lo (default_lang_options ());
    return lo;
  }

  bool
  keyword_p (llvm::StringRef n)
  {
    if (ast_context != 0)
    {
      clang::IdentifierInfo& ii (ast_context->Idents.get (n));
      return ii.isKeyword (ast_context->getLangOpts ());
    }

    static clang::IdentifierTable it (lang_options ());
    return it.get (n).isKeyword (lang_options ());
  }
}

//
// cxx_tokens_lexer
//

void cxx_tokens_lexer::
start (cxx_tokens const& ts, location_t start_loc)
{
  tokens_ = &ts;
  cur_ = ts.begin ();
  loc_ = start_loc;
}

cpp_ttype cxx_tokens_lexer::
next (std::string& token, tree* node)
{
  if (cur_ != tokens_->end ())
  {
    loc_ = cur_->loc;
    token = cur_->literal;
    if (node != 0)
      *node = cur_->node;
    return static_cast<cpp_ttype> (cur_++->type);
  }
  else
    return CPP_EOF;
}

location_t cxx_tokens_lexer::
location () const
{
  return loc_;
}

//
// cxx_string_lexer
//

struct cxx_string_lexer::impl
{
  impl (char const* b, char const* e)
      : lexer (clang::SourceLocation (), odbc::lang_options (), b, b, e)
  {
    lexer.SetCommentRetentionState (false);
  }

  clang::Lexer lexer;
};

cxx_string_lexer::
cxx_string_lexer ()
    : impl_ (0)
{
}

cxx_string_lexer::
~cxx_string_lexer ()
{
  delete impl_;
}

void cxx_string_lexer::
start (string const& data)
{
  data_ = data;
  buf_ = data;
  loc_ = location_t ();

  delete impl_;
  impl_ = new impl (buf_.c_str (), buf_.c_str () + buf_.size ());
}

cpp_ttype cxx_string_lexer::
next (string& token, tree* node)
{
  token.clear ();

  if (node != 0)
    *node = tree ();

  clang::Token t;
  impl_->lexer.LexFromRawLexer (t);

  cpp_ttype tt (odbc::token_type (t.getKind ()));

  switch (tt)
  {
  case CPP_EOF:
    break;
  case CPP_NAME:
    {
      token = t.getRawIdentifier ().str ();

      // See if this is a keyword in the current dialect.
      //
      if (odbc::keyword_p (token))
        tt = CPP_KEYWORD;

      break;
    }
  case CPP_STRING:
  case CPP_NUMBER:
  case CPP_CHAR:
  case CPP_WCHAR:
  case CPP_CHAR16:
  case CPP_CHAR32:
  case CPP_UTF8CHAR:
  case CPP_WSTRING:
  case CPP_STRING16:
  case CPP_STRING32:
  case CPP_UTF8STRING:
    {
      token.assign (t.getLiteralData (), t.getLength ());
      break;
    }
  default:
    {
      if (tt <= CPP_LAST_PUNCTUATOR)
        token = token_spelling[tt];
      else
      {
        cerr << "error: unexpected token in '" << data_ << "'" << endl;
        throw invalid_input ();
      }
      break;
    }
  }

  return tt;
}

location_t cxx_string_lexer::
location () const
{
  return loc_;
}

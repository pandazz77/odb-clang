// file      : odb/cxx-lexer.hxx
// license   : GNU GPL v3; see accompanying LICENSE file

#ifndef ODB_CXX_LEXER_HXX
#define ODB_CXX_LEXER_HXX

#include <odb/clang-fwd.hxx>

#include <string>
#include <cstddef> // std::size_t

#include <odb/cxx-token.hxx>

class cxx_lexer
{
public:
  virtual
  ~cxx_lexer ();

public:
  struct invalid_input {};

  virtual cpp_ttype
  next (std::string& token, tree* node = 0) = 0;

  // Location of the last returned token.
  //
  virtual location_t
  location () const = 0;

public:
  static char const* token_spelling[N_TTYPES + 1];
};


// Adapter to scan a saved token sequence. It returns numbers in the same
// form as they were saved in the token sequence.
//
class cxx_tokens_lexer: public cxx_lexer
{
public:
  void
  start (cxx_tokens const&, location_t start_loc = 0);

  virtual cpp_ttype
  next (std::string& token, tree* node = 0);

  virtual location_t
  location () const;

private:
  cxx_tokens const* tokens_;
  cxx_tokens::const_iterator cur_;
  location_t loc_;
};


// A thin wrapper around the Clang raw lexer for lexing C++ code fragments.
// It returns numbers as string literals.
//
class cxx_string_lexer: public cxx_lexer
{
public:
  cxx_string_lexer ();

  virtual
  ~cxx_string_lexer ();

public:
  void
  start (std::string const&);

  virtual cpp_ttype
  next (std::string& token, tree* node = 0);

  virtual location_t
  location () const;

private:
  cxx_string_lexer (cxx_string_lexer const&);
  cxx_string_lexer& operator= (cxx_string_lexer const&);

  struct impl;

  std::string data_;
  std::string buf_;
  impl* impl_;
  location_t loc_;
};

#endif // ODB_CXX_LEXER_HXX

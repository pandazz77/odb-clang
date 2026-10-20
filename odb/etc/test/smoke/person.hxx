#ifndef PERSON_HXX
#define PERSON_HXX

#include <string>
#include <vector>
#include <memory>
#include <odb/core.hxx>

#pragma db object
class person
{
public:
  person (const std::string& first, const std::string& last, unsigned short age)
      : first_ (first), last_ (last), age_ (age) {}

  const std::string& first () const {return first_;}
  unsigned short age () const {return age_;}

private:
  friend class odb::access;
  person () {}

  #pragma db id auto
  unsigned long id_;

  std::string first_;
  std::string last_;
  unsigned short age_;
  std::vector<std::string> nicknames_;
};

#pragma db view object(person)
struct person_stat
{
  #pragma db column("count(" + person::id_ + ")")
  std::size_t count;

  #pragma db column("min(" + person::age_ + ")")
  unsigned short min_age;
};

#endif

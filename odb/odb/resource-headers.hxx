// file      : odb/resource-headers.hxx
// license   : GNU GPL v3; see accompanying LICENSE file

#ifndef ODB_RESOURCE_HEADERS_HXX
#define ODB_RESOURCE_HEADERS_HXX

#include <cstddef> // std::size_t

// Clang resource headers (stddef.h, etc) embedded into the executable
// (see embed-resources.cmake). The table is terminated with the entry
// that has the NULL path.
//
struct odb_resource_file
{
  const char* path; // Relative to the resource directory (include/...).
  const char* data;
  std::size_t size;
};

extern const odb_resource_file* odb_resource_files;

#endif // ODB_RESOURCE_HEADERS_HXX

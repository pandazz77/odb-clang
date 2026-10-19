// file      : odb/frontend.hxx
// license   : GNU GPL v3; see accompanying LICENSE file

#ifndef ODB_FRONTEND_HXX
#define ODB_FRONTEND_HXX

// The option that switches the odb executable into the compiler frontend
// mode (must be the first argument). The driver re-executes itself in this
// mode for each input file and database.
//
#define ODB_FRONTEND_OPTION "-cc1odb"

#include <string>
#include <vector>

#include <libcutl/fs/path.hxx>

int
frontend_main (int argc, char* argv[]);

// Return the header search paths (in the search order) that the frontend
// would use with the specified compiler options. Return false if the
// paths could not be determined (diagnostics has been issued).
//
bool
frontend_include_paths (std::vector<std::string> const& args,
                        char const* name,
                        std::vector<cutl::fs::path>&);

#endif // ODB_FRONTEND_HXX

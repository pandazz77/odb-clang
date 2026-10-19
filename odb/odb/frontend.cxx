// file      : odb/frontend.cxx
// license   : GNU GPL v3; see accompanying LICENSE file

// The Clang-based compiler frontend. This replaces the GCC plugin: the
// driver (odb.cxx) re-executes itself in the frontend mode passing the
// synthesized translation unit on stdin, the compiler options as is, and
// the ODB options encoded as -fplugin-arg-odb-* (as with the plugin).
//

#include <odb/clang.hxx>

#include <memory>  // std::unique_ptr
#include <string>
#include <vector>
#include <cstdio>
#include <cstring> // std::strcmp, std::strncmp
#include <cassert>
#include <cstdlib> // std::exit
#include <iostream>
#include <iterator>

#ifdef _WIN32
#  include <io.h>    // _setmode
#  include <fcntl.h> // _O_BINARY
#endif

#include <clang/AST/ASTConsumer.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/DiagnosticOptions.h>
#include <clang/Driver/Compilation.h>
#include <clang/Driver/Driver.h>
#include <clang/Driver/Job.h>
#include <clang/Driver/Tool.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/CompilerInvocation.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Frontend/TextDiagnosticPrinter.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Lex/PreprocessorOptions.h>
#include <clang/Sema/SemaConsumer.h>

#include <llvm/Demangle/Demangle.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <clang/Lex/HeaderSearchOptions.h>
#include <clang/Options/Options.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/Support/VersionTuple.h>
#include <llvm/Support/FileSystem.h>

#include <libcutl/re.hxx>
#include <libcutl/fs/path.hxx>

#include <odb/pragma.hxx>
#include <odb/parser.hxx>
#include <odb/options.hxx>
#include <odb/option-functions.hxx>
#include <odb/features.hxx>
#include <odb/profile.hxx>
#include <odb/version.hxx>
#include <odb/validator.hxx>
#include <odb/processor.hxx>
#include <odb/generator.hxx>
#include <odb/resource-headers.hxx>
#include <odb/frontend.hxx>
#include <odb/semantics/unit.hxx>

// libclangSema.a references llvm::itaniumDemangle via dllimport (in some
// distributions, for example, MSYS2, LLVM is built as a DLL). Provide the
// import thunk pointer for the static link.
//
#if defined(_WIN32) && defined(__GNUC__)
using demangle_fn = char* (*) (std::string_view, bool);
extern "C" demangle_fn
__imp__ZN4llvm15itaniumDemangleESt17basic_string_viewIcSt11char_traitsIcEEb;
demangle_fn
__imp__ZN4llvm15itaniumDemangleESt17basic_string_viewIcSt11char_traitsIcEEb =
  &llvm::itaniumDemangle;
#endif

using namespace std;
using namespace semantics;

using cutl::fs::path;
using cutl::fs::invalid_path;

typedef vector<path> paths;
typedef vector<string> strings;

unique_ptr<options const> options_;
paths profile_paths_;
path file_;    // File being compiled.
paths inputs_; // List of input files in at-once mode or just file_.

static int result_ (0);

// Run the ODB compilation pipeline on the parsed translation unit.
//
static int
compile_unit ()
{
  int r (0);

  try
  {
    // Process pragmas (they are collected during parsing but can only be
    // processed once we have the AST).
    //
    process_odb_pragmas ();

    if (errorcount != 0)
      return 1;

    // Post process pragmas.
    //
    post_process_pragmas ();

    // Parse the Clang AST to semantic graph.
    //
    parser p (*options_, loc_pragmas_, ns_loc_pragmas_, decl_pragmas_);
    unique_ptr<unit> u (p.parse (global_namespace, file_));

    features f;

    // Process, pass 1.
    //
    process (*options_, f, *u, file_, 1);

    // Validate, pass 1.
    //
    validate (*options_, f, *u, file_, 1);

    // Process, pass 2.
    //
    process (*options_, f, *u, file_, 2);

    // Validate, pass 2.
    //
    validate (*options_, f, *u, file_, 2);

    // Generate.
    //
    generate (*options_, f, *u, file_, inputs_);
  }
  catch (cutl::re::format const& e)
  {
    cerr << "error: invalid regex: '" << e.regex () << "': " <<
      e.description () << endl;
    r = 1;
  }
  catch (pragmas_failed const&)
  {
    // Diagnostics has aready been issued.
    //
    r = 1;
  }
  catch (parser::failed const&)
  {
    // Diagnostics has aready been issued.
    //
    r = 1;
  }
  catch (validator_failed const&)
  {
    // Diagnostics has aready been issued.
    //
    r = 1;
  }
  catch (processor_failed const&)
  {
    // Diagnostics has aready been issued.
    //
    r = 1;
  }
  catch (generator_failed const&)
  {
    // Diagnostics has aready been issued.
    //
    r = 1;
  }

  return r;
}

namespace
{
  // Count errors and warnings issued by Clang (the ODB code checks
  // errorcount to decide whether to continue).
  //
  struct diagnostic_printer: clang::TextDiagnosticPrinter
  {
    using clang::TextDiagnosticPrinter::TextDiagnosticPrinter;

    virtual void
    HandleDiagnostic (clang::DiagnosticsEngine::Level l,
                      clang::Diagnostic const& d) override
    {
      clang::TextDiagnosticPrinter::HandleDiagnostic (l, d);

      if (l >= clang::DiagnosticsEngine::Error)
        errorcount++;
      else if (l == clang::DiagnosticsEngine::Warning)
        warningcount++;
    }
  };

  struct consumer: clang::SemaConsumer
  {
    virtual void
    InitializeSema (clang::Sema& s) override
    {
      odbc::sema_ = &s;
    }

    virtual void
    ForgetSema () override
    {
      odbc::sema_ = 0;
    }

    virtual void
    HandleTranslationUnit (clang::ASTContext& c) override
    {
      // If there were errors during compilation, don't continue.
      //
      if (errorcount != 0 || c.getDiagnostics ().hasErrorOccurred ())
      {
        result_ = 1;
        return;
      }

      odbc::ast_context = &c;
      odbc::source_manager = &c.getSourceManager ();
      odbc::main_file_id = c.getSourceManager ().getMainFileID ();
      global_namespace = odbc::make (c.getTranslationUnitDecl ());

      result_ = compile_unit ();

      // Similar to the GCC plugin, exit right away without tearing down
      // the (potentially large) AST.
      //
      llvm::errs ().flush ();
      llvm::outs ().flush ();
      cout.flush ();
      cerr.flush ();
      exit (result_);
    }
  };

  struct action: clang::ASTFrontendAction
  {
    virtual unique_ptr<clang::ASTConsumer>
    CreateASTConsumer (clang::CompilerInstance&, llvm::StringRef) override
    {
      return make_unique<consumer> ();
    }

    virtual bool
    BeginSourceFileAction (clang::CompilerInstance& ci) override
    {
      register_odb_pragmas (ci.getPreprocessor ());
      return true;
    }
  };

  // Parse ODB options (encoded as -fplugin-arg-odb-<key>[=<value>]).
  //
  bool
  parse_odb_options (char const* argv0, strings const& pargs)
  {
    strings argv_str;
    argv_str.push_back (argv0);

    for (string const& pa: pargs)
    {
      string a (pa, 17); // Strip -fplugin-arg-odb-.
      string k, v;
      bool has_v (false);

      size_t p (a.find ('='));
      if (p != string::npos)
      {
        k.assign (a, 0, p);
        v.assign (a, p + 1, string::npos);
        has_v = true;
      }
      else
        k = a;

      // A value cannot contain '=' so it is passed as the backspace
      // character.
      //
      for (size_t i (0); i < v.size (); ++i)
        if (v[i] == '\b')
          v[i] = '=';

      // Handle service options.
      //
      if (k == "svc-path")
      {
        profile_paths_.push_back (path (v));
        continue;
      }

      if (k == "svc-file")
      {
        // First is the main file. Subsequent are inputs in the at-once
        // mode.
        //
        if (file_.empty ())
          file_ = path (v);
        else
          inputs_.push_back (path (v));

        continue;
      }

      string opt (k.size () > 1 ? "--" : "-");
      opt += k;

      argv_str.push_back (opt);

      if (has_v)
        argv_str.push_back (v);
    }

    vector<char*> argv;
    for (strings::iterator i (argv_str.begin ()); i != argv_str.end (); ++i)
      argv.push_back (const_cast<char*> (i->c_str ()));

    int argc (static_cast<int> (argv.size ()));

    if (inputs_.empty ())
      inputs_.push_back (file_);

    // Two-phase options parsing, similar to the driver.
    //
    cli::argv_file_scanner::option_info oi[3];
    oi[0].option = "--options-file";
    oi[0].search_func = 0;
    oi[1].option = "-p";
    oi[2].option = "--profile";

    database db;
    {
      oi[1].search_func = &profile_search_ignore;
      oi[2].search_func = &profile_search_ignore;

      cli::argv_file_scanner scan (argc, &argv[0], oi, 3);
      options ops (scan);
      assert (ops.database_specified ());
      db = ops.database ()[0];
    }

    profile_data pd (profile_paths_, db, "odb");
    oi[1].search_func = &profile_search;
    oi[2].search_func = &profile_search;
    oi[1].arg = &pd;
    oi[2].arg = &pd;

    cli::argv_file_scanner scan (argc, &argv[0], oi, 3);
    unique_ptr<options> ops (
      new options (scan, cli::unknown_mode::fail, cli::unknown_mode::fail));

    // Process options.
    //
    process_options (*ops);

    options_ = move (ops);
    pragma_db_ = db;
    pragma_multi_ = options_->multi_database ();

    return true;
  }

  // Add the embedded Clang resource headers to the in-memory file system
  // under the resource directory.
  //
  void
  add_resource_files (llvm::vfs::InMemoryFileSystem& fs, string const& dir)
  {
    for (odb_resource_file const* f (odb_resource_files); f->path != 0; ++f)
    {
      llvm::SmallString<256> p (dir);
      llvm::sys::path::append (p, f->path);
      llvm::sys::path::native (p);

      fs.addFile (p,
                  0,
                  llvm::MemoryBuffer::getMemBuffer (
                    llvm::StringRef (f->data, f->size), p, false));
    }
  }
}

namespace
{
  string
  executable_path (char const* argv0)
  {
    string r (llvm::sys::fs::getMainExecutable (
                argv0, reinterpret_cast<void*> (&frontend_main)));
    return r.empty () ? string (argv0) : r;
  }

  // Resource directory. Use the location the Clang driver would expect
  // relative to our executable but serve the headers from memory.
  //
  string
  resource_directory (string const& exe)
  {
    llvm::SmallString<256> p (llvm::sys::path::parent_path (exe));
    llvm::sys::path::append (p, "..", "lib", "clang");
    llvm::sys::path::append (p, ODB_CLANG_VERSION_MAJOR);
    llvm::sys::path::remove_dots (p, true);
    llvm::sys::path::native (p);
    return p.str ().str ();
  }

  llvm::IntrusiveRefCntPtr<llvm::vfs::OverlayFileSystem>
  file_system (string const& resource_dir)
  {
    llvm::IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> mfs (
      new llvm::vfs::InMemoryFileSystem);
    add_resource_files (*mfs, resource_dir);

    llvm::IntrusiveRefCntPtr<llvm::vfs::OverlayFileSystem> fs (
      new llvm::vfs::OverlayFileSystem (llvm::vfs::getRealFileSystem ()));
    fs->pushOverlay (mfs);
    return fs;
  }

  // Build the compiler invocation for the input file using the Clang
  // driver. Return NULL on failure (diagnostics has been issued).
  //
  std::shared_ptr<clang::CompilerInvocation>
  build_invocation (string const& exe,
                    string const& resource_dir,
                    strings const& cargs,
                    string const& input,
                    clang::DiagnosticsEngine& diags,
                    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> fs,
                    char const* name)
  {
    vector<char const*> dargs;
    dargs.push_back (exe.c_str ());
    dargs.push_back ("-fsyntax-only");
    dargs.push_back ("-resource-dir");
    dargs.push_back (resource_dir.c_str ());

    for (string const& a: cargs)
      dargs.push_back (a.c_str ());

    // The ODB pragmas are handled by us and the translation unit is
    // presented as the main file.
    //
    dargs.push_back ("-Wno-pragma-once-outside-header");
    dargs.push_back (input.c_str ());

    clang::driver::Driver drv (exe, llvm::sys::getDefaultTargetTriple (),
                               diags, "odb", fs);
    drv.setCheckInputsExist (false);

    unique_ptr<clang::driver::Compilation> c (drv.BuildCompilation (dargs));

    if (!c || diags.hasErrorOccurred ())
      return nullptr;

    // Print the commands with -v (similar to what GCC does).
    //
    if (c->getArgs ().hasArg (clang::options::OPT_v))
      c->getJobs ().Print (llvm::errs (), "\n", true);

    clang::driver::Command const* cmd (0);

    for (clang::driver::Command const& j: c->getJobs ())
    {
      if (strcmp (j.getCreator ().getName (), "clang") == 0)
      {
        cmd = &j;
        break;
      }
    }

    if (cmd == 0)
    {
      cerr << name << ": ice: unable to construct compiler invocation"
           << endl;
      return nullptr;
    }

    std::shared_ptr<clang::CompilerInvocation> inv (
      make_shared<clang::CompilerInvocation> ());

    if (!clang::CompilerInvocation::CreateFromArgs (
          *inv, cmd->getArguments (), diags, name))
      return nullptr;

    return inv;
  }

  // Return true if the C++ standard library headers can be found in the
  // header search paths of the invocation.
  //
  bool
  has_std_headers (clang::CompilerInvocation const& inv)
  {
    for (clang::HeaderSearchOptions::Entry const& e:
           inv.getHeaderSearchOpts ().UserEntries)
    {
      llvm::SmallString<256> p (e.Path);
      llvm::sys::path::append (p, "cstddef");

      if (llvm::sys::fs::exists (p))
        return true;
    }

    return false;
  }

#ifdef _WIN32
  // Find the latest MSVC toolset (VC/Tools/MSVC/<version>) installed in the
  // standard Visual Studio locations. Return empty string if not found.
  //
  string
  find_msvc_tools ()
  {
    string r;
    llvm::VersionTuple rv;
    int ry (0);

    for (char const* ev: {"ProgramFiles(x86)", "ProgramFiles"})
    {
      char const* pf (getenv (ev));
      if (pf == 0)
        continue;

      llvm::SmallString<256> vs (pf);
      llvm::sys::path::append (vs, "Microsoft Visual Studio");

      error_code ec;
      for (llvm::sys::fs::directory_iterator yi (vs, ec), ye;
           !ec && yi != ye; yi.increment (ec))
      {
        // <year>/<edition>/VC/Tools/MSVC/<version>
        //
        int y (atoi (llvm::sys::path::filename (yi->path ()).str ().c_str ()));

        error_code ec1;
        for (llvm::sys::fs::directory_iterator ei (yi->path (), ec1), ee;
             !ec1 && ei != ee; ei.increment (ec1))
        {
          llvm::SmallString<256> m (ei->path ());
          llvm::sys::path::append (m, "VC", "Tools", "MSVC");

          error_code ec2;
          for (llvm::sys::fs::directory_iterator vi (m, ec2), ve;
               !ec2 && vi != ve; vi.increment (ec2))
          {
            llvm::VersionTuple v;
            if (v.tryParse (llvm::sys::path::filename (vi->path ())))
              continue;

            llvm::SmallString<256> h (vi->path ());
            llvm::sys::path::append (h, "include", "cstddef");

            if (!llvm::sys::fs::exists (h))
              continue;

            if (r.empty () || y > ry || (y == ry && v > rv))
            {
              r = vi->path ();
              rv = v;
              ry = y;
            }
          }
        }
      }
    }

    return r;
  }
#endif

  bool
  has_arg (strings const& args, char const* prefix)
  {
    size_t n (strlen (prefix));

    for (string const& a: args)
      if (a.compare (0, n, prefix) == 0)
        return true;

    return false;
  }

  // Create the compiler invocation. On Windows, if the C++ standard
  // library headers cannot be found for the default (MinGW) target and
  // no target was specified explicitly, fall back to the MSVC target and
  // toolset, if installed. This way we don't require a GCC installation
  // to parse the headers.
  //
  std::shared_ptr<clang::CompilerInvocation>
  create_invocation (string const& exe,
                     string const& resource_dir,
                     strings const& cargs,
                     string const& input,
                     clang::DiagnosticsEngine& diags,
                     llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> fs,
                     char const* name)
  {
#ifdef _WIN32
    bool target (has_arg (cargs, "--target=") || has_arg (cargs, "-target"));
    bool tools (has_arg (cargs, "-Xmicrosoft-visualc-tools-root") ||
                has_arg (cargs, "/vctoolsdir") ||
                getenv ("VCToolsInstallDir") != 0);

    strings args (cargs);

    // If the MSVC target is specified explicitly, then help Clang find the
    // toolset (the Visual Studio setup API is not available in all Clang
    // builds).
    //
    if (target && !tools && (has_arg (cargs, "--target=x86_64-pc-windows-msvc") ||
                             has_arg (cargs, "--target=aarch64-pc-windows-msvc") ||
                             has_arg (cargs, "--target=i686-pc-windows-msvc")))
    {
      string t (find_msvc_tools ());

      if (!t.empty ())
      {
        args.push_back ("-Xmicrosoft-visualc-tools-root");
        args.push_back (t);
      }
    }

    std::shared_ptr<clang::CompilerInvocation> inv (
      build_invocation (exe, resource_dir, args, input, diags, fs, name));

    if (inv == nullptr || target || tools || has_std_headers (*inv))
      return inv;

    string t (find_msvc_tools ());

    if (t.empty ())
      return inv;

    llvm::Triple tt (llvm::sys::getDefaultTargetTriple ());

    args.push_back ("--target=" + tt.getArchName ().str () +
                    "-pc-windows-msvc");
    args.push_back ("-Xmicrosoft-visualc-tools-root");
    args.push_back (t);

    // MSVC's standard library requires C++14 or later.
    //
    for (string& a: args)
    {
      if (a == "-std=c++0x" || a == "-std=c++11")
        a = "-std=c++14";
    }

    if (options_ != nullptr && options_->trace ())
      cerr << "no C++ standard library headers found for the default "
           << "target, using MSVC from " << t << endl;

    return build_invocation (exe, resource_dir, args, input, diags, fs, name);
#else
    return build_invocation (exe, resource_dir, cargs, input, diags, fs, name);
#endif
  }
}

bool
frontend_include_paths (strings const& cargs,
                        char const* name,
                        paths& r)
{
  string exe (executable_path (name));
  string resource_dir (resource_directory (exe));

  clang::DiagnosticOptions dopts;
  clang::DiagnosticsEngine diags (
    llvm::IntrusiveRefCntPtr<clang::DiagnosticIDs> (new clang::DiagnosticIDs),
    dopts,
    new clang::TextDiagnosticPrinter (llvm::errs (), dopts));

  std::shared_ptr<clang::CompilerInvocation> inv (
    create_invocation (exe,
                       resource_dir,
                       cargs,
                       "odb-profile.cxx",
                       diags,
                       file_system (resource_dir),
                       name));

  if (inv == nullptr)
    return false;

  // Order the paths the same way as the header search does: quoted,
  // angled, system, and after.
  //
  clang::HeaderSearchOptions const& hso (inv->getHeaderSearchOpts ());

  using clang::frontend::IncludeDirGroup;
  IncludeDirGroup const groups[] = {
    clang::frontend::Quoted,
    clang::frontend::Angled,
    clang::frontend::System,
    clang::frontend::ExternCSystem,
    clang::frontend::CSystem,
    clang::frontend::CXXSystem,
    clang::frontend::ObjCSystem,
    clang::frontend::ObjCXXSystem,
    clang::frontend::After};

  for (IncludeDirGroup g: groups)
  {
    for (clang::HeaderSearchOptions::Entry const& e: hso.UserEntries)
    {
      if (e.Group == g && !e.IsFramework)
      {
        try
        {
          r.push_back (path (e.Path));
        }
        catch (invalid_path const&) {}
      }
    }
  }

  return true;
}

int
frontend_main (int argc, char* argv[])
{
  // argv[0] is the executable, argv[1] is the frontend mode option.
  //
  char const* argv0 (argv[0]);

  string exe (executable_path (argv0));

  // Expand response files (see fixup_cmd_line() in odb.cxx).
  //
  llvm::BumpPtrAllocator alloc;
  llvm::SmallVector<char const*, 256> av (argv + 2, argv + argc);
  {
    llvm::cl::ExpansionContext ec (alloc, llvm::cl::TokenizeGNUCommandLine);
    if (llvm::Error err = ec.expandResponseFiles (av))
    {
      cerr << argv0 << ": error: " << llvm::toString (move (err)) << endl;
      return 1;
    }
  }

  // Separate the compiler and ODB options.
  //
  strings cargs, pargs;
  bool stdin_input (false);

  for (char const* a: av)
  {
    if (strncmp (a, "-fplugin-arg-odb-", 17) == 0)
      pargs.push_back (a);
    else if (strcmp (a, "-") == 0)
      stdin_input = true;
    else
      cargs.push_back (a);
  }

  if (!stdin_input)
  {
    cerr << argv0 << ": ice: expected translation unit on stdin" << endl;
    return 1;
  }

  try
  {
    if (!parse_odb_options (argv0, pargs))
      return 1;
  }
  catch (cli::exception const& ex)
  {
    cerr << ex << endl;
    return 1;
  }
  catch (invalid_path const& ex)
  {
    cerr << argv0 << ": error: invalid path '" << ex.path () << "'" << endl;
    return 1;
  }

  if (options_->trace ())
    cerr << "starting frontend " << argv0 << endl;

  // Read the translation unit.
  //
#ifdef _WIN32
  _setmode (_fileno (stdin), _O_BINARY);
#endif

  string tu;
  {
    char buf[8192];
    for (size_t n; (n = fread (buf, 1, sizeof (buf), stdin)) != 0; )
      tu.append (buf, n);

    if (ferror (stdin))
    {
      cerr << argv0 << ": error: unable to read translation unit" << endl;
      return 1;
    }
  }

  // The main file path. We make the translation unit appear as the actual
  // input file (or the input name in the at-once mode) so that relative
  // inclusion and #pragma once work.
  //
  string main_file;
  {
    llvm::SmallString<256> p (file_.string ());
    llvm::sys::fs::make_absolute (p);
    llvm::sys::path::remove_dots (p, true);
    llvm::sys::path::native (p);
    main_file = p.str ().str ();
  }

  string resource_dir (resource_directory (exe));
  llvm::IntrusiveRefCntPtr<llvm::vfs::OverlayFileSystem> fs (
    file_system (resource_dir));

  // Diagnostics.
  //
  clang::DiagnosticOptions dopts;
  dopts.ShowColors = false;
  diagnostic_printer* dp (new diagnostic_printer (llvm::errs (), dopts));
  clang::DiagnosticsEngine diags (
    llvm::IntrusiveRefCntPtr<clang::DiagnosticIDs> (new clang::DiagnosticIDs),
    dopts,
    dp);

  std::shared_ptr<clang::CompilerInvocation> inv (
    create_invocation (exe, resource_dir, cargs, main_file, diags, fs, argv0));

  if (inv == nullptr)
    return 1;

  // Serve the translation unit from memory.
  //
  inv->getPreprocessorOpts ().addRemappedFile (
    main_file,
    llvm::MemoryBuffer::getMemBufferCopy (tu, main_file).release ());
  inv->getPreprocessorOpts ().RetainRemappedFileBuffers = false;

  inv->getFrontendOpts ().DisableFree = true;

  clang::CompilerInstance ci (inv);
  ci.createVirtualFileSystem (fs, dp);
  ci.createDiagnostics (dp, false);

  action a;
  bool ok (ci.ExecuteAction (a));

  // We normally exit from the consumer. Getting here means compilation
  // errors.
  //
  llvm::errs ().flush ();

  if (!ok || errorcount != 0)
    return 1;

  return result_;
}

// Database.cpp
//
// The Database and Statement classes are fully implemented in Database.h as
// inline definitions.  This .cpp exists so CMake can list it as a source file
// that contributes to the object library / executable without requiring any
// additional compilation units.  It also serves as the single translation unit
// that pulls in <sqlite3.h>, ensuring the amalgamation's implementation symbols
// (sqlite3.c compiled into the sqlite3 static library) are only linked once.
//
// Phase scope: Database is a thin RAII shell.  No connection pooling,
// prepared-statement cache, or WAL configuration here — those belong in Phase 4+.
#include "Database.h"

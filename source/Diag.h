#pragma once

#include <string>

/**
    Logging for a plugin that lives inside somebody else's process.

    A small member of the fleet's `diag` family. The rest of the repos get a
    rotating log, a crash report and a diagnostics bundle; an FFGL effect gets
    only the log, for two reasons:

    - **No crash handler.** A plugin loaded into Resolume must not install a
      process-wide signal handler. It would intercept faults that are not ours
      and interfere with the host's own handling. A plugin has no business
      deciding what happens when Resolume dies.
    - **No bundle command.** There is no UI to hang one off -- an effect is a
      list of sliders in someone else's inspector.

    What it covers are the two failures that actually happen.

    **A shader that will not compile.** `InitGL` returns `FF_FAIL` and from
    the operator's side that looks like "the effect does nothing", with no
    message anywhere; with four passes this also records *which* one, which is
    the difference between a five-minute fix and an afternoon. The GL vendor
    and version strings go in next to it, because a shader that compiles on one
    machine and not on another is a driver answer, not a source answer.

    **A frame queue that could not be allocated.** This plugin holds up to
    thirty-two full pictures at once, and the failure mode when the driver
    says no is a black frame with nothing to explain it. What goes in the log
    is the size asked for and how much it came to, because the fix is almost
    always Resolution or Frames rather than anything in the code.
*/
namespace afterglow::diag
{

/// Open the log file and record the plugin build, once per process.
void init();

void info( const std::string& message );
void warn( const std::string& message );
void error( const std::string& message );

/// Full path of the log file, for the README to point at.
std::string logPath();

} // namespace afterglow::diag

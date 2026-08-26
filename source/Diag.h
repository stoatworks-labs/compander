#pragma once

#include <string>

/**
    Logging for a plugin that lives inside somebody else's process.

    The fleet's usual small `diag`: a log file and nothing else. No crash
    handler -- a plugin loaded into Resolume has no business installing a
    process-wide signal handler and intercepting faults that are not its own --
    and no diagnostics bundle, because an effect is a list of sliders in
    somebody else's inspector and there is no UI to hang one off.

    What it covers is the two failures that actually happen.

    **A shader that will not compile.** `InitGL` returns `FF_FAIL` and from the
    operator's side that is "the effect does nothing", with no message anywhere.
    Compander has eight shader stages and three of them are assembled at runtime
    from more than one string, so the log says which stage, what the compiler
    said, and the assembled source -- because a line number in that message
    refers to the assembled string and to nothing you can open in an editor.

    **A buffer the driver would not allocate.** Compander asks for five
    full-resolution float buffers, and the log records the size it asked for,
    because the fix is almost always the composition resolution.

        ~/Library/Logs/compander/compander.YYYY-MM-DD.log
*/
namespace compander::diag
{

/// Open the log file and record the plugin build, once per process.
void init();

void info( const std::string& message );
void warn( const std::string& message );
void error( const std::string& message );

/// A shader stage that would not compile: the stage's name and the assembled
/// source the compiler's line numbers refer to.
///
/// Deliberately does NOT reach for glGetString. This file is GL-free so that
/// the OpenFX build -- which has no GL anywhere in it -- can link it; the
/// caller logs the vendor and version alongside.
void shaderFailed( const char* stage, const char* source );

/// A buffer allocation the driver refused, with the size that was asked for.
void allocationFailed( int width, int height );

/// Full path of the log file, for the README to point at.
std::string logPath();

} // namespace compander::diag

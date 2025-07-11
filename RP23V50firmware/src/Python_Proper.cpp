#include "Python_Proper.h"
#include <Arduino.h>
#include <FatFS.h>
#include "config.h"
#include "FilesystemStuff.h"
extern "C" {
#include "py/gc.h"

#include "py/runtime.h"
#include "py/stackctrl.h"
#include "py/mpstate.h"
#include "py/repl.h"
#include <micropython_embed.h>
}

// Global state for proper MicroPython integration
static char mp_heap[32 * 1024]; // 32KB heap for MicroPython (reduced for RP2350B)
static bool mp_initialized = false;
static bool mp_repl_active = false;
static bool jumperless_globals_loaded = false;

// Command execution state
static char mp_command_buffer[512];
static bool mp_command_ready = false;
static char mp_response_buffer[1024];

// Terminal colors for different REPL states
/// 0 = menu (cyan) 1 = prompt (light blue) 2 = output (chartreuse) 3 = input
/// (orange-yellow) 4 = error (orange-red) 5 = purple 6 = dark purple 7 = light
/// cyan 8 = magenta 9 = pink 10 = green 11 = grey 12 = dark grey 13 = light
/// grey
static int replColors[15] = {
    38,  // menu (cyan)
    69,  // prompt (light blue)
    155, // output (chartreuse)
    221, // input (orange-yellow)
    202, // error (orange-red)
    92,  // purple
    56,  // dark purple
    51,  // light cyan
    199, // magenta
    207, // pink
    40,  //  green
    8,   // grey
    235, // dark grey
    248, // light grey

};

Stream *global_mp_stream = &Serial;

// C-compatible pointer for HAL functions
extern "C" {
    void *global_mp_stream_ptr = (void *)&Serial;
}

// Forward declaration for color function (from Graphics.cpp)
void changeTerminalColor(int termColor, bool flush, Stream *stream);

// Forward declarations
extern "C" {
void mp_hal_stdout_tx_strn_cooked(const char *str, size_t len);
int mp_hal_stdin_rx_chr(void);
mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len);
void mp_hal_delay_ms(mp_uint_t ms);
mp_uint_t mp_hal_ticks_ms(void);

// Arduino wrapper functions for HAL
int arduino_serial_available(Stream *stream = global_mp_stream);
int arduino_serial_read(Stream *stream = global_mp_stream);
void arduino_serial_write(const char *str, int len, void *stream);
void arduino_delay_ms(unsigned int ms);
unsigned int arduino_millis(void);

// Export global_mp_stream for C code
extern void *global_mp_stream_ptr;
}

// Arduino timing functions for MicroPython
extern "C" void mp_hal_delay_ms(mp_uint_t ms) { delay(ms); }

extern "C" mp_uint_t mp_hal_ticks_ms(void) { return millis(); }

// Arduino wrapper functions for the HAL layer
extern "C" int arduino_serial_available(Stream *stream) {
  return global_mp_stream->available();
}

extern "C" int arduino_serial_read(Stream *stream) {
  return global_mp_stream->read();
}

extern "C" void arduino_serial_write(const char *str, int len, void *stream) {
  Stream *s = (Stream *)stream;
  if (s) {
    // Convert \n to \r\n for proper terminal display
    for (int i = 0; i < len; i++) {

      s->write(str[i]);

      if (str[i] == '\n') {
        s->write('\r');
      }
    }
    s->flush();
  }
}

extern "C" void arduino_delay_ms(unsigned int ms) { delay(ms); }

extern "C" unsigned int arduino_millis() { return millis(); }

void setGlobalStream(Stream *stream) {
  global_mp_stream = stream;
  global_mp_stream_ptr = (void *)stream;
}

// Terminal color control function is now in Graphics.cpp

// MicroPython HAL stdout function with Jumperless-specific functionality
extern "C" void mp_hal_stdout_tx_strn_cooked(const char *str, size_t len) {
    // Basic output to global stream (regular MicroPython output)
    if (global_mp_stream) {
        for (size_t i = 0; i < len; i++) {
            if (str[i] == '\n') {
                global_mp_stream->write('\r');
            }
            global_mp_stream->write(str[i]);
        }
        global_mp_stream->flush();
    }
}

// HAL functions are now implemented in lib/micropython/port/mphalport.c




bool initMicroPythonProper(Stream *stream) {
  // global_mp_stream = stream;

  if (mp_initialized) {
    return true;
  }

  global_mp_stream->println(
      "[MP] Initializing MicroPython...");

  // Get proper stack pointer
  char stack_dummy;
  char *stack_top = &stack_dummy;
    // Set up Python path and basic modules (with error handling)
  // mp_embed_exec_str(
  //     "try:\n"
  //     "    print('MicroPython initializing...')\n"
  //     "    import sys\n"
  //     "    sys.path.append('/lib')\n"
  //     "    print('MicroPython', sys.version)\n"
  //     "except ImportError as e:\n"
  //     "    print('MicroPython initialized (sys module unavailable)')\n"
  //     "print('\\nJumperless hardware control available')\n");
  // changeTerminalColor(replColors[5], true, global_mp_stream);
  // // Test if native jumperless module is available
  // mp_embed_exec_str(
  //     "try:\n"
  //     "    import jumperless\n"
  //     // "    import time\n"
  //     "    print('Native jumperless module imported successfully')\n"
  //     "    print('Available functions:')\n"
  //     "    for func in dir(jumperless):\n"
  //     "        if not func.startswith('_'):\n"
  //     "            print('  jumperless.' + func + '()')\n"
  //     "    print()  # Empty line\n"
  //     "except ImportError as e:\n"
  //     "    print('❌ Native jumperless module not available:', str(e))\n"
  //     "    print('Loading Python wrapper functions instead...')\n"
  //     "    print()  # Empty line\n");
  // Initialize MicroPython
  mp_embed_init(mp_heap, sizeof(mp_heap), stack_top);

  // Simple initialization - don't load complex modules during startup
  mp_embed_exec_str("print('MicroPython ready for Jumperless')");
  
  // Manually set up basic path since sys.path is disabled to avoid build errors
  // This allows importing from /python directory when using FatFS
    mp_initialized = true;
  mp_repl_active = false;
  mp_embed_exec_str(
      "try:\n"
      "    import sys\n"
      "    # Note: sys.path is disabled in build, but sys module works\n"
      "    print('Python path: /python (FatFS)')\n"
      "except ImportError:\n"
      "    print('sys module not available')\n");

  addJumperlessPythonFunctions();
  addMicroPythonModules();


  global_mp_stream->println("[MP] MicroPython initialized successfully");
  return true;
}

void deinitMicroPythonProper(void) {
  if (mp_initialized) {
    global_mp_stream->println("[MP] Deinitializing MicroPython...");
    mp_embed_deinit();
      mp_initialized = false;
  mp_repl_active = false;
  jumperless_globals_loaded = false;  // Reset globals flag
  }
}

bool executePythonCodeProper(const char *code) {
  if (!mp_initialized) {
    global_mp_stream->println("[MP] Error: MicroPython not initialized");
    return false;
  }

  if (!code || strlen(code) == 0) {
    return false;
  }

  // Clear response buffer
  memset(mp_response_buffer, 0, sizeof(mp_response_buffer));

  // Execute the code with proper error handling
  // MicroPython handles errors internally and prints them
  mp_embed_exec_str(code);
  return true;
}

void startMicroPythonREPL(void) {
  if (!mp_initialized) {
    changeTerminalColor(replColors[4], true, global_mp_stream);
    global_mp_stream->println("[MP] Error: MicroPython not initialized");
    return;
  }

  if (mp_repl_active) {
    changeTerminalColor(replColors[4], true, global_mp_stream);
    global_mp_stream->println("[MP] REPL already active");
    return;
  }

  // Print Python prompt with color
  changeTerminalColor(replColors[1], true, global_mp_stream);
  global_mp_stream->print(
      ">>> "); // Simple prompt - the processMicroPythonInput handles everything
  global_mp_stream->flush();

  mp_repl_active = true;
}

void stopMicroPythonREPL(void) {
  if (mp_repl_active) {
    changeTerminalColor(0, false, global_mp_stream);
    global_mp_stream->println("\n[MP] Exiting REPL...");
    mp_repl_active = false;
  }
}

bool isMicroPythonREPLActive(void) { return mp_repl_active; }

void enterMicroPythonREPL(Stream *stream) {
  // Colorful initialization like original implementation
  changeTerminalColor(replColors[6], true, global_mp_stream);

  // Initialize MicroPython if not already done
  if (!mp_initialized) {
    if (!initMicroPythonProper()) {
      changeTerminalColor(replColors[4], true, global_mp_stream); // error color
      global_mp_stream->println("Failed to initialize MicroPython!");
      return;
    }
  }
  
  // Always add jumperless functions to global namespace when entering REPL
  // This makes all functions available without the jumperless. prefix
  addJumperlessPythonFunctions();

  // Check if REPL is already active
  if (mp_repl_active) {
    changeTerminalColor(replColors[4], true, global_mp_stream);
    global_mp_stream->println("[MP] REPL already active");
    return;
  }

  // Show colorful welcome messages
  // changeTerminalColor(replColors[7], true,global_mp_stream);
  // global_mp_stream->println("MicroPython REPL with embedded Jumperless
  // hardware control!"); global_mp_stream->println("Type normal Python code,
  // then press Enter to execute"); global_mp_stream->println("Use TAB for
  // indentation (or exactly 4 spaces)"); global_mp_stream->println("Use ↑/↓
  // arrows for command history, ←/→ arrows for cursor movement");
  // global_mp_stream->println("Navigate multiline code with ← to beginning of
  // lines"); global_mp_stream->println("Type help_jumperless() for hardware
  // control commands");

  changeTerminalColor(replColors[0], true, global_mp_stream);
  global_mp_stream->println("MicroPython initialized successfully");
  global_mp_stream->flush();
  delay(200);

  changeTerminalColor(replColors[0], true, global_mp_stream);
  global_mp_stream->println();
  changeTerminalColor(replColors[2], true, global_mp_stream);
  global_mp_stream->println("    MicroPython REPL");

  // Show commands menu
  changeTerminalColor(replColors[5], true, global_mp_stream);
  global_mp_stream->println("\n Commands:");
  changeTerminalColor(replColors[3], false, global_mp_stream);
  global_mp_stream->print("  quit ");
  changeTerminalColor(replColors[0], false, global_mp_stream);
  global_mp_stream->println("       -   Exit REPL");
  changeTerminalColor(replColors[3], false, global_mp_stream);
  global_mp_stream->print("  helpl");
  changeTerminalColor(replColors[0], false, global_mp_stream);
  global_mp_stream->println("       -   Show REPLhelp");
  changeTerminalColor(replColors[3], false, global_mp_stream);
  global_mp_stream->print("  history ");
  changeTerminalColor(replColors[0], false, global_mp_stream);
  global_mp_stream->println("    -   Show command history");
  changeTerminalColor(replColors[3], false, global_mp_stream);
  global_mp_stream->print("  save ");
  changeTerminalColor(replColors[0], false, global_mp_stream);
  global_mp_stream->println("       -   Save last script");
  changeTerminalColor(replColors[3], false, global_mp_stream);
  global_mp_stream->print("  load ");
  changeTerminalColor(replColors[0], false, global_mp_stream);
  global_mp_stream->println("       -   Load saved script");
  // changeTerminalColor(replColors[3], false, global_mp_stream);
  // global_mp_stream->print("  delete ");
  // changeTerminalColor(replColors[0], false, global_mp_stream);
  // global_mp_stream->println("     -   Delete saved script");
  changeTerminalColor(replColors[3], false, global_mp_stream);
  global_mp_stream->print("  files ");
  changeTerminalColor(replColors[0], false, global_mp_stream);
  global_mp_stream->println("      -   Open file manager (python_scripts)");
  changeTerminalColor(replColors[3], false, global_mp_stream);
  global_mp_stream->print("  new ");
  changeTerminalColor(replColors[0], false, global_mp_stream);
  global_mp_stream->println("        -   Create new script with eKilo editor");
  changeTerminalColor(replColors[3], false, global_mp_stream);
  global_mp_stream->print("  multiline ");
  changeTerminalColor(replColors[0], false, global_mp_stream);
  global_mp_stream->println("  -   Toggle multiline mode");
  changeTerminalColor(replColors[3], false, global_mp_stream);
  global_mp_stream->print("  run ");
  changeTerminalColor(replColors[0], false, global_mp_stream);
  global_mp_stream->println("        -   Execute script (multiline mode)");
  changeTerminalColor(replColors[3], false, global_mp_stream);
  global_mp_stream->print("  help() ");
  changeTerminalColor(replColors[0], false, global_mp_stream);
  global_mp_stream->println("     - Show hardware commands");

  changeTerminalColor(replColors[5], true, global_mp_stream);
  global_mp_stream->println("\nNavigation:");
  changeTerminalColor(replColors[8], false, global_mp_stream);
  global_mp_stream->println("  ↑/↓ arrows - Browse command history");
  global_mp_stream->println("  ←/→ arrows - Move cursor, edit text");
  global_mp_stream->println("  TAB        - Add 4-space indentation");
  global_mp_stream->println(
      "  Enter      - Execute (empty line in multiline to finish)");
  // global_mp_stream->println("  files      - Browse and manage Python scripts");
  // global_mp_stream->println("  new        - Create new scripts with eKilo editor");
  // global_mp_stream->println("  run        - Execute accumulated script "
  //                           "(multiline forced ON)");

  changeTerminalColor(replColors[5], true, global_mp_stream);
  global_mp_stream->println("\nHardware:");
  changeTerminalColor(replColors[7], false, global_mp_stream);
  global_mp_stream->println(
      "  help()  - Show Jumperless hardware commands");
  global_mp_stream->println(
      "  ");
  global_mp_stream->println();

  if (global_mp_stream == &Serial) {
    global_mp_stream->write(0x0E); // turn on interactive mode
    global_mp_stream->flush();
  }

  // Wait for user to press enter
  changeTerminalColor(replColors[4], true, global_mp_stream);
  global_mp_stream->print("\n\rPress enter to start REPL");
  global_mp_stream->println();
  global_mp_stream->flush();
  while (global_mp_stream->available() == 0) {
    delay(1);
  }
  global_mp_stream->read(); // consume the enter keypress

  // Start the REPL with colors
  changeTerminalColor(replColors[1], true, global_mp_stream);
  startMicroPythonREPL();

  // Blocking loop - stay in REPL until user exits
  while (mp_repl_active) {
    processMicroPythonInput(global_mp_stream);
    delayMicroseconds(10); // Small delay to prevent overwhelming
  }

  // Cleanup with colors
  changeTerminalColor(replColors[0], true, global_mp_stream);
  global_mp_stream->println("\nExiting REPL...");
  if (global_mp_stream == &Serial) {
    global_mp_stream->write(0x0F); // turn off interactive mode
    global_mp_stream->flush();
  }
  global_mp_stream->print("\033[0m");
  // stream->println("Returned to Arduino mode");
}

void processMicroPythonInput(Stream *stream) {
  if (!mp_initialized) {
    return;
  }

  // // Process any queued hardware commands
  // if (mp_command_ready) {
  //     global_mp_stream->printf("[MP] Processing hardware command: %s\n",
  //     mp_command_buffer);

  //     // Execute the hardware command through your existing system
  //     char response[512];
  //     int result = parseAndExecutePythonCommand(mp_command_buffer, response);

  //     // Make result available to Python
  //     if (result == 0) {
  //         String python_result = "globals()['_last_result'] = '";
  //         python_result += response;
  //         python_result += "'";
  //         mp_embed_exec_str(python_result.c_str());
  //     }

  //     mp_command_ready = false;
  // }

  // Handle REPL input with proper text editor functionality and history
  if (mp_repl_active) {
    static REPLEditor editor;
    static ScriptHistory history;
    static bool history_initialized = false;

    if (editor.first_run) {
      // Initialize history first, before any input processing
      if (!history_initialized) {
        history.initFilesystem();
        history_initialized = true;
      }

      // Start with a fresh prompt
      changeTerminalColor(replColors[1], true, global_mp_stream);
      global_mp_stream->flush();
      editor.first_run = false;
    }

    // Check for available input
    if (global_mp_stream->available()) {
      int c = global_mp_stream->read();

      // Character processing for escape sequences

      // Handle escape sequences for arrow keys
      if (editor.escape_state == 0 && c == 27) { // ESC
        editor.escape_state = 1;
        return;
      } else if (editor.escape_state == 1 && c == 91) { // [
        editor.escape_state = 2;
        return;
      } else if (editor.escape_state == 2) {
        editor.escape_state = 0; // Reset escape state

        switch (c) {
        case 65: // Up arrow - history previous
        {
          String prev_cmd = history.getPreviousCommand();
          if (prev_cmd.length() > 0) {
            editor.loadFromHistory(global_mp_stream, prev_cmd);
            global_mp_stream->flush();
          }
        }
          return;

        case 66: // Down arrow - history next
        {
          String next_cmd = history.getNextCommand();
          if (next_cmd.length() > 0) {
            editor.loadFromHistory(global_mp_stream, next_cmd);
            global_mp_stream->flush();
            } else {
            // Return to original input
            editor.exitHistoryMode(global_mp_stream);
            global_mp_stream->flush();
          }
        }
          return;

        case 67: // Right arrow
          if (editor.cursor_pos < editor.current_input.length()) {
            editor.cursor_pos++;
            global_mp_stream->print("\033[C"); // Move cursor right
            global_mp_stream->flush();
          }
          return;

        case 68: // Left arrow
          if (editor.cursor_pos > 0) {
            // Exit history mode when user starts navigating
            if (editor.in_history_mode) {
              editor.in_history_mode = false;
              history.resetHistoryNavigation();
            }
            
            char char_to_left = editor.current_input.charAt(editor.cursor_pos - 1);
            
            if (char_to_left == '\n') {
              // Navigate to previous line
              editor.navigateOverNewline(global_mp_stream);
            } else {
              // Normal left movement
              editor.cursor_pos--;
              global_mp_stream->print("\033[D"); // Move cursor left
              global_mp_stream->flush();
            }
          }
          return;

        default:
          // Unknown escape sequence - just ignore it
          return;
        }
      } else if (editor.escape_state > 0) {
        // We're in the middle of an escape sequence but got an unexpected
        // character
        editor.escape_state = 0; // Reset escape state
        // Don't process this character as regular input
        return;
      }

      // Handle Ctrl+C (ASCII 3) - cancel current input and reset
      if (c == 3) {
        global_mp_stream->println("^C");
        global_mp_stream->println("KeyboardInterrupt");
        editor.reset();
        changeTerminalColor(replColors[1], true, global_mp_stream);
        global_mp_stream->print(">>> ");
        global_mp_stream->flush();
        return;
      }

      // Handle Enter key - check for multiline or execute
      if (c == '\r' || c == '\n') {
        global_mp_stream->println(); // Echo newline

        // Check for special commands first
        String trimmed_input = editor.current_input;
        trimmed_input.trim();

        //! Exit commands
        if (trimmed_input == "exit()" || trimmed_input == "quit()" ||
            trimmed_input == "exit" || trimmed_input == "quit") {
          stopMicroPythonREPL();
          editor.reset();
          return;
        }

        //! History commands
        if (trimmed_input == "history" || trimmed_input == "history()") {
          history.listScripts();
          editor.reset();
          changeTerminalColor(replColors[1], true, global_mp_stream);
          global_mp_stream->print(">>> ");
          global_mp_stream->flush();
          return;
        }

        //! Multiline mode commands
        if (trimmed_input == "multiline" || trimmed_input == "multiline()") {
          global_mp_stream->println("Multiline mode status:");
          if (editor.multiline_forced_on) {
            global_mp_stream->println("  Currently: FORCED ON");
          } else if (editor.multiline_forced_off) {
            global_mp_stream->println("  Currently: FORCED OFF");
          } else {
            global_mp_stream->println("  Currently: AUTO (default)");
          }
          global_mp_stream->println("Commands:");
          global_mp_stream->println("  multiline on   - Force multiline mode "
                                    "ON (use 'run' to execute)");
          global_mp_stream->println(
              "  multiline off  - Force multiline mode OFF");
          global_mp_stream->println(
              "  multiline auto - Return to automatic detection");
          editor.reset();
          changeTerminalColor(replColors[1], true, global_mp_stream);
          global_mp_stream->print(">>> ");
          global_mp_stream->flush();
          return;
        }

        if (trimmed_input == "multiline on") {
          editor.multiline_forced_on = true;
          editor.multiline_forced_off = false;
          editor.multiline_override = true;
          global_mp_stream->println("Multiline mode: FORCED ON");
          changeTerminalColor(replColors[7], false, global_mp_stream);
          global_mp_stream->println("Enter will add new lines. Type 'run' to "
                                    "execute accumulated script.");
          editor.reset();
          changeTerminalColor(replColors[1], true, global_mp_stream);
          global_mp_stream->print(">>> ");
          global_mp_stream->flush();
          return;
        }

        if (trimmed_input == "multiline off") {
          editor.multiline_forced_on = false;
          editor.multiline_forced_off = true;
          editor.multiline_override = true;
          global_mp_stream->println("Multiline mode: FORCED OFF");
          editor.reset();
          changeTerminalColor(replColors[1], true, global_mp_stream);
          global_mp_stream->print(">>> ");
          global_mp_stream->flush();
          return;
        }

        if (trimmed_input == "multiline auto") {
          editor.multiline_forced_on = false;
          editor.multiline_forced_off = false;
          editor.multiline_override = false;
          global_mp_stream->println("Multiline mode: AUTO (default)");
          editor.reset();
          changeTerminalColor(replColors[1], true, global_mp_stream);
          global_mp_stream->print(">>> ");
          global_mp_stream->flush();
          return;
        }



        //! New command - create new script with eKilo editor
        if (trimmed_input == "new" || trimmed_input == "new()") {
          changeTerminalColor(replColors[5], true, global_mp_stream);
          global_mp_stream->println("Opening eKilo editor...");
          changeTerminalColor(replColors[0], false, global_mp_stream);
          
          // Launch eKilo editor in REPL mode
          String savedContent = launchEkiloREPL(nullptr);
          
          // Restore interactive mode after returning from eKilo
          if (global_mp_stream == &Serial) {
            global_mp_stream->write(0x0E); // turn on interactive mode
            global_mp_stream->flush();
          }
          
          // If content was saved, load it into the editor
          if (savedContent.length() > 0) {
            editor.current_input = savedContent;
            editor.cursor_pos = savedContent.length();
            editor.in_multiline_mode = (savedContent.indexOf('\n') >= 0);
            changeTerminalColor(replColors[5], true, global_mp_stream);
            global_mp_stream->println("Script content loaded into REPL");
            // Don't reset - show the loaded content
            editor.redrawFullInput(global_mp_stream);
            return;
          } else {
            changeTerminalColor(replColors[5], true, global_mp_stream);
            global_mp_stream->println("Returned from eKilo editor");
            
            editor.reset();
            changeTerminalColor(replColors[1], true, global_mp_stream);
            global_mp_stream->print(">>> ");
            global_mp_stream->flush();
            return;
          }
        }

        //! Help commands
        if (trimmed_input == "helpl" || trimmed_input == "helpl()") {
          // Show REPL help
          changeTerminalColor(replColors[7], true, global_mp_stream);
          global_mp_stream->println("\n   MicroPython REPL Help");
          changeTerminalColor(replColors[5], true, global_mp_stream);
          global_mp_stream->println("\nCommands:");
          changeTerminalColor(replColors[3], false, global_mp_stream);
          global_mp_stream->print("  quit/exit ");
          changeTerminalColor(replColors[0], false, global_mp_stream);
          global_mp_stream->println("    -   Exit REPL");
          changeTerminalColor(replColors[3], false, global_mp_stream);
          global_mp_stream->print("  history ");
          changeTerminalColor(replColors[0], false, global_mp_stream);
          global_mp_stream->println(
              "      -   Show command history & saved scripts");
          changeTerminalColor(replColors[3], false, global_mp_stream);
          global_mp_stream->print("  save [name] ");
          changeTerminalColor(replColors[0], false, global_mp_stream);
          global_mp_stream->println(
              "  -   Save last script (auto: script_N.py)");
          changeTerminalColor(replColors[3], false, global_mp_stream);
          global_mp_stream->print("  load <name> ");
          changeTerminalColor(replColors[0], false, global_mp_stream);
          global_mp_stream->println("  -   Load script by name");
          changeTerminalColor(replColors[3], false, global_mp_stream);
          global_mp_stream->print("  delete <name>");
          changeTerminalColor(replColors[0], false, global_mp_stream);
          global_mp_stream->println(" -   Delete saved script");
          changeTerminalColor(replColors[3], false, global_mp_stream);
          global_mp_stream->print("  files ");
          changeTerminalColor(replColors[0], false, global_mp_stream);
          global_mp_stream->println("       -   Open file manager (python_scripts)");
          changeTerminalColor(replColors[3], false, global_mp_stream);
          global_mp_stream->print("  new ");
          changeTerminalColor(replColors[0], false, global_mp_stream);
          global_mp_stream->println("         -   Create new script with eKilo editor");
          changeTerminalColor(replColors[3], false, global_mp_stream);
          global_mp_stream->print("  multiline ");
          changeTerminalColor(replColors[0], false, global_mp_stream);
          global_mp_stream->println(
              "    -   Toggle multiline mode (on/off/auto)");
          changeTerminalColor(replColors[3], false, global_mp_stream);
          global_mp_stream->print("  run ");
          changeTerminalColor(replColors[0], false, global_mp_stream);
          global_mp_stream->println(
              "          -   Execute script (when multiline forced ON)");

          changeTerminalColor(replColors[5], true, global_mp_stream);
          global_mp_stream->println("\nNavigation:");
          changeTerminalColor(replColors[8], false, global_mp_stream);
          global_mp_stream->println("  ↑/↓ arrows - Browse command history");
          global_mp_stream->println("  ←/→ arrows - Move cursor, edit text");
          global_mp_stream->println("  TAB        - Add 4-space indentation");
          global_mp_stream->println(
              "  Enter      - Execute (empty line in multiline to finish)");
          global_mp_stream->println("  files      - Browse and manage Python scripts");
          global_mp_stream->println("  new        - Create new scripts with eKilo editor");
          global_mp_stream->println("  run        - Execute accumulated script "
                                    "(multiline forced ON)");

          changeTerminalColor(replColors[5], true, global_mp_stream);
          global_mp_stream->println("\nHardware:");
          changeTerminalColor(replColors[7], false, global_mp_stream);
          global_mp_stream->println(
              "  help()  - Show Jumperless hardware commands");
          global_mp_stream->println(
              "  ");
          global_mp_stream->println();

          editor.reset();
          changeTerminalColor(replColors[1], true, global_mp_stream);
          global_mp_stream->print(">>> ");
          global_mp_stream->flush();
          return;
        }

        //! Save command - save the last executed script from history
        if (trimmed_input.startsWith("save ") || trimmed_input == "save" ||
            trimmed_input == "save()") {
          // Get the most recent executed script from history
          String last_script = history.getLastExecutedCommand();
          if (last_script.length() > 0) {
            String filename = "";
            if (trimmed_input.startsWith("save ")) {
              filename = trimmed_input.substring(5);
              filename.trim();
            }
            if (history.saveScript(last_script, filename)) {
              global_mp_stream->println("Script saved to filesystem");
            } else {
              global_mp_stream->println("Failed to save script");
            }
          } else {
            global_mp_stream->println("No previous script to save");
          }
          editor.reset();
          changeTerminalColor(replColors[1], true, global_mp_stream);
          global_mp_stream->print(">>> ");
          global_mp_stream->flush();
          return;
        }

        //! Load command - load a script from filesystem
        if (trimmed_input.startsWith("load ") || trimmed_input == "load") {
          if (trimmed_input == "load") {
            // Show available scripts when no filename provided
            global_mp_stream->println("Available scripts:");
            history.listScripts();
            String recent_script = history.getLastSavedScript();
            if (recent_script.length() > 0) {
              global_mp_stream->println("Most recent: " + recent_script);
              global_mp_stream->println("Try: load " + recent_script);
            }
            global_mp_stream->println(
                "Usage: load <number> or load <filename>");
          } else {
            String arg = trimmed_input.substring(5);
            arg.trim();
            if (arg.length() > 0) {
              String filename = "";

              // Check if argument is a number
              bool is_number = true;
              for (int i = 0; i < arg.length(); i++) {
                if (!isdigit(arg.charAt(i))) {
                  is_number = false;
                  break;
                }
              }

              if (is_number) {
                // Handle numeric input
                int script_number = arg.toInt();
                if (script_number >= 1 &&
                    script_number <= history.getNumberedScriptsCount()) {
                  filename = history.getNumberedScript(
                      script_number - 1); // Convert 1-based to 0-based
                  global_mp_stream->println("Loading script " +
                                            String(script_number) + ": " +
                                            filename);
                } else {
                  global_mp_stream->println(
                      "Invalid script number. Use 'history' to see available "
                      "scripts.");
                  editor.reset();
                  changeTerminalColor(replColors[1], true, global_mp_stream);
                  global_mp_stream->print(">>> ");
                  global_mp_stream->flush();
                  return;
                }
              } else {
                // Handle filename input
                filename = arg;
              }

              if (filename.length() > 0) {
                String loaded_script = history.loadScript(filename);
                if (loaded_script.length() > 0) {
                  // Load the script into the editor
                  editor.current_input = loaded_script;
                  editor.cursor_pos = loaded_script.length();
                  editor.in_multiline_mode = (loaded_script.indexOf('\n') >= 0);
                  editor.redrawFullInput(global_mp_stream);
                  return; // Stay in editing mode
                }
              }
            } else {
              global_mp_stream->println(
                  "Usage: load <number> or load <filename>");
            }
          }
          editor.reset();
          changeTerminalColor(replColors[1], true, global_mp_stream);
          global_mp_stream->print(">>> ");
          global_mp_stream->flush();
          return;
        }

        //! Delete command - delete a script from filesystem
        if (trimmed_input.startsWith("delete ") ||
            trimmed_input.startsWith("del ")) {
          int start_pos = trimmed_input.startsWith("delete ") ? 7 : 4;
          String filename = trimmed_input.substring(start_pos);
          filename.trim();
          if (filename.length() > 0) {
            history.deleteScript(filename);
          } else {
            global_mp_stream->println("Usage: delete filename");
          }
          editor.reset();
          changeTerminalColor(replColors[1], true, global_mp_stream);
          global_mp_stream->print(">>> ");
          global_mp_stream->flush();
          return;
        }

        //! Files command - launch file manager in python_scripts directory
        if (trimmed_input == "files" || trimmed_input == "files()" ||
            trimmed_input == "filemanager" || trimmed_input == "filemanager()") {
          changeTerminalColor(replColors[5], true, global_mp_stream);
          global_mp_stream->println("Opening file manager...");
          changeTerminalColor(replColors[0], false, global_mp_stream);
          
          // Launch file manager in REPL mode
          String savedContent = filesystemAppPythonScriptsREPL();
          
          // Restore interactive mode after returning from file manager
          if (global_mp_stream == &Serial) {
            global_mp_stream->write(0x0E); // turn on interactive mode
            global_mp_stream->flush();
          }
          
          // If content was saved, load it into the editor
          if (savedContent.length() > 0) {
            editor.current_input = savedContent;
            editor.cursor_pos = savedContent.length();
            editor.in_multiline_mode = (savedContent.indexOf('\n') >= 0);
            changeTerminalColor(replColors[5], true, global_mp_stream);
            global_mp_stream->println("File content loaded into REPL");
            // Don't reset - show the loaded content
            editor.redrawFullInput(global_mp_stream);
            return;
          } else {
            changeTerminalColor(replColors[5], true, global_mp_stream);
            global_mp_stream->println("Returned from file manager");
            editor.reset();
            changeTerminalColor(replColors[1], true, global_mp_stream);
            global_mp_stream->print(">>> ");
            global_mp_stream->flush();
            return;
          }
        }

        //! Special handling for forced multiline mode
        if (editor.multiline_forced_on) {
          // Check if the user typed 'run' (as the only content or last line)
          if (trimmed_input == "run") {
            // Only "run" was typed - no script to execute
            global_mp_stream->println("No script to execute.");
            editor.reset();
            changeTerminalColor(replColors[1], true, global_mp_stream);
            global_mp_stream->print(">>> ");
            global_mp_stream->flush();
            return;
          } else if (trimmed_input.endsWith("\nrun") &&
                     trimmed_input.length() > 4) {
            // Script followed by 'run' command
            String script_to_execute =
                trimmed_input.substring(0, trimmed_input.length() - 4);
            script_to_execute.trim();

            if (script_to_execute.length() > 0) {
              changeTerminalColor(replColors[2], true, global_mp_stream);
              global_mp_stream->println("Executing accumulated script:");

              // Execute the user's current input (edited or original)
              // No longer override with history command - user edits should be respected

              // Add to history before execution
              history.addToHistory(script_to_execute);

              // Reset history navigation now that we're executing
              history.resetHistoryNavigation();

              // Execute the complete script
              mp_embed_exec_str(script_to_execute.c_str());
            }

            // Reset and show new prompt
            editor.reset();
            changeTerminalColor(replColors[1], true, global_mp_stream);
            global_mp_stream->print(">>> ");
            global_mp_stream->flush();
            return;
          }
          // If not 'run', force multiline continuation (never execute
          // individual lines)
        }

        // Check if this is an empty line in multiline mode (escape mechanism)
        bool force_execution = false;
        if (editor.in_multiline_mode && !editor.multiline_forced_on) {
          // Only allow empty line escape in AUTO mode, not when forced ON
          int line_start =
              editor.current_input.lastIndexOf('\n', editor.cursor_pos - 1);
          line_start = (line_start >= 0) ? line_start + 1 : 0;
          String current_line =
              editor.current_input.substring(line_start, editor.cursor_pos);
          current_line.trim();
          if (current_line.length() == 0) {
            force_execution = true; // Empty line in multiline mode
          }
        }

        // Check if MicroPython needs more input (multiline detection)
        // Use current input WITHOUT adding newline first
        bool needs_more_input = false;
        if (editor.multiline_forced_on) {
          // In forced ON mode, ALWAYS continue - never execute until 'run' is
          // typed
          needs_more_input = true;
        } else if (editor.current_input.length() > 0 && !force_execution) {
          if (editor.multiline_forced_off) {
            // In forced OFF mode, NEVER continue (always execute on Enter)
            needs_more_input = false;
          } else {
            // Use automatic detection (default behavior)
            String input_for_check = editor.current_input;
            needs_more_input =
                mp_repl_continue_with_input(input_for_check.c_str());
          }
        }

        if (needs_more_input && !force_execution) {
          editor.in_multiline_mode = true;

          // Add newline at cursor position since we need more input
          editor.current_input =
              editor.current_input.substring(0, editor.cursor_pos) + "\n" +
              editor.current_input.substring(editor.cursor_pos);
          editor.cursor_pos++;

          // Smart auto-indent: maintain or increase indentation level
          // Get the line we just finished (before the newline we just added)
          String lines = editor.current_input;
          int last_newline = lines.lastIndexOf(
              '\n',
              editor.cursor_pos - 2); // -2 to skip the newline we just added
          String last_line = "";
          if (last_newline >= 0) {
            last_line = lines.substring(
                last_newline + 1,
                editor.cursor_pos - 1); // exclude the newline we just added
          } else {
            last_line = lines.substring(
                0, editor.cursor_pos - 1); // first line, exclude newline
          }

          // Calculate current indentation level of the previous line
          int current_indent = 0;
          for (int i = 0; i < last_line.length(); i++) {
            if (last_line.charAt(i) == ' ') {
              current_indent++;
            } else {
              break;
            }
          }

          String trimmed_last_line = last_line;
          trimmed_last_line.trim();

          String indent_spaces = "";
          if (trimmed_last_line.endsWith(":")) {
            // Increase indentation level by 4 spaces
            for (int i = 0; i < current_indent + 4; i++) {
              indent_spaces += " ";
            }
          } else if (current_indent > 0) {
            // Maintain current indentation level
            for (int i = 0; i < current_indent; i++) {
              indent_spaces += " ";
            }
          }

          // Insert the indentation at cursor position
          editor.current_input =
              editor.current_input.substring(0, editor.cursor_pos) +
              indent_spaces + editor.current_input.substring(editor.cursor_pos);
          editor.cursor_pos += indent_spaces.length();

          // Show the appropriate prompt
          changeTerminalColor(replColors[1], true, global_mp_stream);
          if (editor.multiline_forced_on) {
            global_mp_stream->print("... "); // Standard multiline prompt
            // Show reminder on first few lines
            static int line_count = 0;
            line_count++;
            if (line_count < 1) { // Show on first multiline prompt
              changeTerminalColor(replColors[12], false, global_mp_stream);
              global_mp_stream->print(" (type 'run' to execute)");
              global_mp_stream->println();
              changeTerminalColor(replColors[1], true, global_mp_stream);
              global_mp_stream->print("... ");
            }
          } else {
            global_mp_stream->print("... ");
          }

          if (indent_spaces.length() > 0) {
            changeTerminalColor(replColors[3], false, global_mp_stream);
            global_mp_stream->print(indent_spaces); // Show the indentation
          }
          global_mp_stream->flush();
        } else {
          // Execute the complete statement (or force execution)
          if (editor.current_input.length() > 0) {
            changeTerminalColor(replColors[2], true, global_mp_stream);

            // Clean up the input (remove trailing newlines)
            String clean_input = editor.current_input;
            while (clean_input.endsWith("\n")) {
              clean_input = clean_input.substring(0, clean_input.length() - 1);
            }

            if (clean_input.length() > 0) {
              // Execute the user's current input (edited or original)
              // No longer override with history command - user edits should be respected

              // Add to history before execution
              history.addToHistory(clean_input);

              // Reset history navigation now that we're executing
              history.resetHistoryNavigation();

              // Let MicroPython handle the complete statement
              mp_embed_exec_str(clean_input.c_str());
            }

            changeTerminalColor(replColors[1], true, global_mp_stream);
          }

          // Reset and show new prompt
          editor.reset();
          changeTerminalColor(replColors[1], true, global_mp_stream);
          global_mp_stream->print(">>> ");
          global_mp_stream->flush();
        }

      } else if (c == '\b' || c == 127) { // Backspace
        if (editor.cursor_pos > 0) {
          // Exit history mode when user starts editing
          if (editor.in_history_mode) {
            editor.in_history_mode = false;
            history.resetHistoryNavigation();
          }
          char char_to_delete =
              editor.current_input.charAt(editor.cursor_pos - 1);

          if (char_to_delete == '\n') {
            // Use the proper backspace over newline function
            editor.backspaceOverNewline(global_mp_stream);
          } else {
            // Check if we're backspacing over a complete tab (4 spaces)
            bool is_tab_backspace = false;
            if (editor.cursor_pos >= 4) {
              String potential_tab = editor.current_input.substring(
                  editor.cursor_pos - 4, editor.cursor_pos);
              if (potential_tab == "    ") {
                // Check if these 4 spaces are at the start of a line or after
                // other whitespace
                int line_start = editor.current_input.lastIndexOf(
                    '\n', editor.cursor_pos - 1);
                line_start = (line_start >= 0) ? line_start + 1 : 0;
                String line_before_cursor = editor.current_input.substring(
                    line_start, editor.cursor_pos - 4);

                // If line before cursor is all whitespace, treat as tab
                bool all_whitespace = true;
                for (int i = 0; i < line_before_cursor.length(); i++) {
                  if (line_before_cursor.charAt(i) != ' ') {
                    all_whitespace = false;
                    break;
                  }
                }

                if (all_whitespace) {
                  is_tab_backspace = true;
                  // Remove 4 spaces at once
                  editor.current_input.remove(editor.cursor_pos - 4, 4);
                  editor.cursor_pos -= 4;
                  editor.redrawCurrentLine(global_mp_stream);
                }
              }
            }

            if (!is_tab_backspace) {
              // Normal single character backspace
              editor.current_input.remove(editor.cursor_pos - 1, 1);
              editor.cursor_pos--;
              editor.redrawCurrentLine(global_mp_stream);
            }
          }
        }
      } else if (c == '\t') { // TAB character
        // Convert TAB to 4 spaces at cursor position
        String spaces = "   ";
        editor.current_input =
            editor.current_input.substring(0, editor.cursor_pos) + spaces +
            editor.current_input.substring(editor.cursor_pos);
        editor.cursor_pos += 4;
        changeTerminalColor(replColors[3], false, global_mp_stream);
        global_mp_stream->print(spaces); // Show 4 spaces
        global_mp_stream->flush();
      } else if (c >= 32 && c <= 126) { // Printable characters
        // Exit history mode when user starts editing
        if (editor.in_history_mode) {
          editor.in_history_mode = false;
          history.resetHistoryNavigation();
        }

        // Insert character at cursor position
        editor.current_input =
            editor.current_input.substring(0, editor.cursor_pos) + (char)c +
            editor.current_input.substring(editor.cursor_pos);
        editor.cursor_pos++;

        // Redraw from cursor position if we're in the middle of text
        if (editor.cursor_pos < editor.current_input.length()) {
          editor.redrawCurrentLine(global_mp_stream);
        } else {
          // Just echo the character if at end
          changeTerminalColor(replColors[3], false, global_mp_stream);
          global_mp_stream->write(c);
          global_mp_stream->flush();
        }
      } else {
        mp_repl_continue_with_input(editor.current_input.c_str());
      }
      // All other characters are ignored
    }
  }
}

// Helper function to add complete Jumperless hardware module
void addNodeConstantsToGlobalNamespace(void) {
  if (!mp_initialized) {
    return;
  }
  
  // This function is now redundant since addJumperlessPythonFunctions() 
  // does 'from jumperless import *' which imports everything.
  // Keeping this for backward compatibility, but just calls the main function.
  addJumperlessPythonFunctions();
}

void testGlobalImports(void) {
  if (!mp_initialized) {
    return;
  }
  
  mp_embed_exec_str(
      "print('Testing global imports...')\n"
      "print('oled_connect available:', 'oled_connect' in globals())\n"
      "print('connect available:', 'connect' in globals())\n"
      "print('TOP_RAIL available:', 'TOP_RAIL' in globals())\n"
      "print('D13 available:', 'D13' in globals())\n"
      "print('jumperless module available:', 'jumperless' in globals())\n");
}

void addJumperlessPythonFunctions(void) {
  if (!mp_initialized) {
    return;
  }
  
  // Only load once to avoid redundant imports
  if (jumperless_globals_loaded) {
    if (global_mp_stream) {
      global_mp_stream->println("[DEBUG] Jumperless globals already loaded, skipping");
    }
    return;
  }
  
  // Debug: print that this function is being called
  if (global_mp_stream) {
    global_mp_stream->println("[DEBUG] Loading jumperless globals for first time");
  }

  // Import jumperless module and add ALL functions and constants to global namespace
  mp_embed_exec_str(
      "try:\n"
      "    print('Attempting to import jumperless module...')\n"
      "    import jumperless\n"
      "    print('Native jumperless module available')\n"
      "    funcs = [attr for attr in dir(jumperless) if not attr.startswith('_')]\n"
      "    #print('Available functions: ' + str(funcs))\n"
      "    \n"
      "    # Import all jumperless functions into global namespace\n"
      "    # This eliminates the need for jumperless. prefix\n"
      "    #print('Importing all functions globally...')\n"
      "    from jumperless import *\n"
      "    \n"
      "    # Also keep jumperless module available for explicit access if needed\n"
      "    globals()['jumperless'] = jumperless\n"
      "    \n"
      "    # Test that functions are actually available\n"
      "    #available_functions = [name for name in globals() if not name.startswith('_') and callable(globals()[name])]\n"
      "    #print(' Available global functions: ' + str(len(available_functions)))\n"
      "    #if 'oled_connect' in globals():\n"
      "    #    print(' oled_connect() is available globally')\n"
      "    #else:\n"
      "    #    print(' oled_connect() not found in globals')\n"
      "    \n"
      "    #print('All jumperless functions and constants available globally')\n"
      "    #print('You can now use: connect(), dac_set(), TOP_RAIL, D13, etc.')\n"
      "    \n"
      "except ImportError as e:\n"
      "    print('△ Native jumperless module not available: ' + str(e))\n"
      "except Exception as e:\n"
      "    print('△ Error setting up globals: ' + str(e))\n"
      "    import traceback\n"
      "    traceback.print_exc()\n");
  
  // Mark as successfully loaded
  jumperless_globals_loaded = true;
}

void addMicroPythonModules(bool time, bool machine, bool os, bool math, bool gc) {
  if (!mp_initialized) {
    return;
  }
  
  if (time) {
    mp_embed_exec_str("import time\n");
    mp_embed_exec_str("print('Time module imported successfully')\n");
  }
  // if (machine) {
  //   mp_embed_exec_str("import machine\n");
  //   mp_embed_exec_str("print('Machine module imported successfully')\n");
  // }
  if (os) {
    mp_embed_exec_str("import os\n");
    mp_embed_exec_str("print('OS module imported successfully')\n");
  }
  if (math) {
    mp_embed_exec_str("import math\n");
    mp_embed_exec_str("print('Math module imported successfully')\n");
  }
  if (gc) {
    mp_embed_exec_str("import gc\n");
    mp_embed_exec_str("print('GC module imported successfully')\n");
  }
}

const char *test_code = R"""(
try:
    import jumperless
            print("☺ Native jumperless module imported successfully")
    
    # Test that functions exist
    if hasattr(jumperless, 'dac_set') and hasattr(jumperless, 'adc_get'):
        print("☺ Core DAC/ADC functions found")
    else:
        print("☹ Core DAC/ADC functions missing")
        
    if hasattr(jumperless, 'nodes_connect') and hasattr(jumperless, 'gpio_set'):
        print("☺ Node and GPIO functions found")
    else:
        print("☹ Node and GPIO functions missing")
        
    if hasattr(jumperless, 'oled_print') and hasattr(jumperless, 'ina_get_current'):
                print("☺ OLED and INA functions found")
    else:
        print("☹ OLED and INA functions missing")
    
    print("☺ Native Jumperless module test completed successfully")
    
except ImportError as e:
    print("☹ Failed to import native jumperless module:", str(e))
except Exception as e:
    print("☹ Error testing native jumperless module:", str(e))
)""";
// Simple execution function for one-off commands
bool executePythonSimple(const char *code, char *response,
                         size_t response_size) {
  if (!mp_initialized) {
    if (response)
      strncpy(response, "ERROR: MicroPython not initialized",
              response_size - 1);
    return false;
  }

  // Clear response buffer
  memset(mp_response_buffer, 0, sizeof(mp_response_buffer));

  // Execute and capture any output
  bool success = executePythonCodeProper(code);

  // Copy response if buffer provided
  if (response && response_size > 0) {
    if (success) {
      global_mp_stream->println("[MP] Executing native module test...");
      mp_embed_exec_str(test_code);
      global_mp_stream->println("[MP] Native module test complete");
    }
  }

  return success;
}

// Status functions
bool isMicroPythonInitialized(void) { return mp_initialized; }

void printMicroPythonStatus(void) {
  global_mp_stream->println("\n=== MicroPython Status ===");
  global_mp_stream->printf("Initialized: %s\n", mp_initialized ? "Yes" : "No");
  global_mp_stream->printf("REPL Active: %s\n", mp_repl_active ? "Yes" : "No");
  global_mp_stream->printf("Heap Size: %d bytes\n", sizeof(mp_heap));

  if (mp_initialized) {
    // Get memory info
    mp_embed_exec_str(
        "import gc; print(f'Free: {gc.mem_free()}, Used: {gc.mem_alloc()}')");
  }
  global_mp_stream->println("=========================\n");
}

// Test function to verify the native Jumperless module is working
void testJumperlessNativeModule(void) {
  if (!mp_initialized) {
    global_mp_stream->println(
        "[MP] Error: MicroPython not initialized for module test");
    return;
  }

  global_mp_stream->println("[MP] Testing native Jumperless module...");

  // Simple test to verify the module can be imported and functions are
  // accessible

  global_mp_stream->println("[MP] Executing native module test...");
  mp_embed_exec_str(test_code);
  global_mp_stream->println("[MP] Native module test complete");
}

// Test function to verify stream redirection is working
void testStreamRedirection(Stream *newStream) {
  if (!mp_initialized) {
    global_mp_stream->println(
        "[MP] Error: MicroPython not initialized for stream test");
    return;
  }

  Stream *oldStream = global_mp_stream;

  // Test output to original stream
  global_mp_stream->println("[MP] Testing stream redirection...");
  global_mp_stream->println("[MP] This should appear on the original stream");
  mp_embed_exec_str("print('Python output to original stream')");

  // Change to new stream using proper setter
  setGlobalStream(newStream);
  newStream->println(
      "[MP] Stream changed - this should appear on the new stream");
  mp_embed_exec_str("print('Python output to new stream')");

  // Change back to original stream using proper setter
  setGlobalStream(oldStream);
  oldStream->println("[MP] Stream changed back - this should appear on the "
                     "original stream again");
  mp_embed_exec_str("print('Python output back to original stream')");

  global_mp_stream->println("[MP] Stream redirection test complete");
}

// ScriptHistory method implementations
void ScriptHistory::initFilesystem() {
  // Note: FatFS should already be initialized by main application
  // Do not call FatFS.begin() here as it can interfere with config loading
  
  // Create scripts directory if it doesn't exist
  if (!FatFS.exists(scripts_dir)) {
    if (!FatFS.mkdir(scripts_dir)) {
      global_mp_stream->println("Failed to create scripts directory");
      return;
    }
  }

  // Load existing history from file
  loadHistoryFromFile();

  // Find the next available script number
  findNextScriptNumber();
}

void ScriptHistory::addToHistory(const String &script) {
  if (script.length() == 0)
    return;

  // Check if this command already exists anywhere in history
  for (int i = 0; i < history_count; i++) {
    if (history[i] == script) {
      // Move this command to the end (most recent)
      String temp = history[i];
      for (int j = i; j < history_count - 1; j++) {
        history[j] = history[j + 1];
      }
      history[history_count - 1] = temp;
      current_history_index = -1; // Reset navigation
      return;
    }
  }

  // Shift history if full
  if (history_count >= MAX_HISTORY) {
    for (int i = 0; i < MAX_HISTORY - 1; i++) {
      history[i] = history[i + 1];
    }
    history_count = MAX_HISTORY - 1;
  }

  history[history_count++] = script;
  current_history_index = -1; // Reset navigation
  saveHistoryToFile();
}

String ScriptHistory::getPreviousCommand() {
  if (history_count == 0)
    return "";

  if (current_history_index == -1) {
    current_history_index = history_count - 1;
  } else if (current_history_index > 0) {
    current_history_index--;
  }
  // If already at the oldest command, stay there

  return history[current_history_index];
}

String ScriptHistory::getNextCommand() {
  if (history_count == 0 || current_history_index == -1)
    return "";

  if (current_history_index < history_count - 1) {
    current_history_index++;
    return history[current_history_index];
  } else {
    // Moving forward past the newest command returns to original input
    current_history_index = -1;
    return ""; // Return to current input
  }
}

String ScriptHistory::getCurrentHistoryCommand() {
  if (current_history_index >= 0 && current_history_index < history_count) {
    return history[current_history_index];
  }
  return "";
}

void ScriptHistory::resetHistoryNavigation() { 
  current_history_index = -1; 
}

void ScriptHistory::clearHistory() {
  history_count = 0;
  current_history_index = -1;
  saveHistoryToFile();
}

String ScriptHistory::getLastExecutedCommand() {
  if (history_count == 0)
    return "";
  return history[history_count - 1]; // Return most recent without affecting navigation
}

String ScriptHistory::getLastSavedScript() { 
  return last_saved_script; 
}

int ScriptHistory::getNextScriptNumber() { 
  return next_script_number; 
}

int ScriptHistory::getNumberedScriptsCount() { 
  return numbered_scripts_count; 
}

String ScriptHistory::getNumberedScript(int index) {
  if (index >= 0 && index < numbered_scripts_count) {
    return numbered_scripts[index];
  }
  return "";
}

bool ScriptHistory::saveScript(const String &script, const String &filename) {
  String fname = filename;
  if (fname.length() == 0) {
    // Generate sequential filename
    fname = "script_" + String(next_script_number);

    // Make sure this filename doesn't already exist, increment if needed
    String fullPath = scripts_dir + "/" + fname + ".py";
    while (FatFS.exists(fullPath)) {
      next_script_number++;
      fname = "script_" + String(next_script_number);
      fullPath = scripts_dir + "/" + fname + ".py";
    }
    next_script_number++; // Increment for next time
  }
  if (!fname.endsWith(".py")) {
    fname += ".py";
  }

  String fullPath = scripts_dir + "/" + fname;
  File file = FatFS.open(fullPath, "w");
  if (!file) {
    global_mp_stream->println("Failed to create script file: " + fullPath);
    return false;
  }

  file.print(script);
  file.close();

  last_saved_script = fname; // Store for easy reference

  // Add to saved scripts list (avoid duplicates)
  bool already_exists = false;
  for (int i = 0; i < saved_scripts_count; i++) {
    if (saved_scripts[i] == fname) {
      already_exists = true;
      break;
    }
  }

  if (!already_exists && saved_scripts_count < 10) {
    saved_scripts[saved_scripts_count++] = fname;
  }

  global_mp_stream->println("Script saved as: " + fullPath);
  addToHistory(script); // Also add to memory history
  return true;
}

String ScriptHistory::loadScript(const String &filename) {
  String fullPath = scripts_dir + "/" + filename;
  if (!filename.endsWith(".py")) {
    fullPath += ".py";
  }

  if (!FatFS.exists(fullPath)) {
    global_mp_stream->println("Script not found: " + fullPath);
    return "";
  }

  File file = FatFS.open(fullPath, "r");
  if (!file) {
    global_mp_stream->println("Failed to open script file: " + fullPath);
    return "";
  }

  String content = file.readString();
  file.close();

  global_mp_stream->println("Script loaded: " + fullPath);
  return content;
}

bool ScriptHistory::deleteScript(const String &filename) {
  String fullPath = scripts_dir + "/" + filename;

  if (filename.startsWith("history")) {
    fullPath = scripts_dir + "/history.txt";
    global_mp_stream->println("Deleting history file: " + fullPath);
    clearHistory();
    return true;
  }


  if (!filename.endsWith(".py")) {
    //fullPath += ".py";
  }

  if (!FatFS.exists(fullPath)) {
    global_mp_stream->println("Script not found: " + fullPath);
    return false;
  }

  if (FatFS.remove(fullPath)) {
    // Remove from saved scripts tracking
    for (int i = 0; i < saved_scripts_count; i++) {
      String saved_name = saved_scripts[i];
      if (!saved_name.endsWith(".py")) {
        //saved_name += ".py";
      }
      String check_name = filename;
      if (!check_name.endsWith(".py")) {
       // check_name += ".py";
      }

      if (saved_name == check_name) {
        // Shift remaining scripts down
        for (int j = i; j < saved_scripts_count - 1; j++) {
          saved_scripts[j] = saved_scripts[j + 1];
        }
        saved_scripts_count--;
        break;
      }
    }

    global_mp_stream->println("Script deleted: " + fullPath);
    return true;
  } else {
    global_mp_stream->println("Failed to delete script: " + fullPath);
    return false;
  }
}

void ScriptHistory::listScripts() {
  changeTerminalColor(replColors[9], false, global_mp_stream);

  // Reset numbered scripts mapping
  numbered_scripts_count = 0;

  // Show recent command history (without numbers)
  changeTerminalColor(replColors[6], false, global_mp_stream);
  global_mp_stream->println("\n\rRecent Commands:");
  changeTerminalColor(replColors[9], false, global_mp_stream);
  for (int i = history_count - 1; i >= 0 && i >= history_count - 5;
       i--) { // Show last 5
    String history_line = history[i].substring(0, 60);
    history_line.replace("\n", "\n\r");
    if (history_line.length() > 60)
      history_line += "...";
    global_mp_stream->printf("   %s\n\r", history_line.c_str());
    if (history[i].length() > 60)
      global_mp_stream->println("...");
  }
  if (history_count == 0) {
    global_mp_stream->println("   No commands in history");
  }

  // Show saved script files with numbers
  changeTerminalColor(replColors[6], false, global_mp_stream);
  global_mp_stream->println("\n\rSaved Scripts:");
  changeTerminalColor(replColors[8], false, global_mp_stream);
  if (!FatFS.exists(scripts_dir)) {
    global_mp_stream->println("   No scripts directory");
    return;
  }

  int script_count = 0;

  // First, show scripts we know we saved in this session
  for (int i = 0; i < saved_scripts_count; i++) {
    String fullPath = scripts_dir + "/" + saved_scripts[i];
    if (!saved_scripts[i].endsWith(".py")) {
      fullPath += ".py";
    }

    if (FatFS.exists(fullPath)) {
      File file = FatFS.open(fullPath, "r");
      if (file) {
        if (numbered_scripts_count < 20) {
          numbered_scripts[numbered_scripts_count] = saved_scripts[i];
          String display_name = saved_scripts[i];
          if (!display_name.endsWith(".py")) {
            display_name += ".py";
          }
          global_mp_stream->printf("   %d. %s (%d bytes) [recent]\n\r",
                                   numbered_scripts_count + 1,
                                   display_name.c_str(), file.size());
          numbered_scripts_count++;
          script_count++;
        }
        file.close();
      }
    }
  }

  // Check for sequential numbered scripts that aren't tracked in memory
  for (int i = 1; i <= 50; i++) { // Check script_1.py through script_50.py
    String script_name = "script_" + String(i);
    String test_script = scripts_dir + "/" + script_name + ".py";

    if (FatFS.exists(test_script)) {
      // Check if we already listed this one
      bool already_listed = false;
      for (int j = 0; j < saved_scripts_count; j++) {
        if (saved_scripts[j] == script_name ||
            (saved_scripts[j] + ".py") == (script_name + ".py")) {
          already_listed = true;
          break;
        }
      }

      if (!already_listed && numbered_scripts_count < 20) {
        File file = FatFS.open(test_script, "r");
        if (file) {
          numbered_scripts[numbered_scripts_count] = script_name;
          global_mp_stream->printf("   %d. %s.py (%d bytes)\n\r",
                                   numbered_scripts_count + 1,
                                   script_name.c_str(), file.size());
          numbered_scripts_count++;
          script_count++;
          file.close();
        }
      }
    }
  }

  // Also check for some common named scripts that might exist
  String common_names[] = {"test",  "demo", "main",
                           "setup", "loop", "example"};
  int num_common = sizeof(common_names) / sizeof(common_names[0]);

  for (int i = 0; i < num_common; i++) {
    String test_script = scripts_dir + "/" + common_names[i] + ".py";
    if (FatFS.exists(test_script)) {
      // Check if we already listed this one
      bool already_listed = false;
      for (int j = 0; j < saved_scripts_count; j++) {
        if (saved_scripts[j] == common_names[i] ||
            (saved_scripts[j] + ".py") == (common_names[i] + ".py")) {
          already_listed = true;
          break;
        }
      }

      if (!already_listed && numbered_scripts_count < 20) {
        File file = FatFS.open(test_script, "r");
        if (file) {
          numbered_scripts[numbered_scripts_count] = common_names[i];
          global_mp_stream->printf("   %d. %s.py (%d bytes)\n\r",
                                   numbered_scripts_count + 1,
                                   common_names[i].c_str(), file.size());
          numbered_scripts_count++;
          script_count++;
          file.close();
        }
      }
    }
  }

  if (script_count == 0) {
    global_mp_stream->println("   No saved scripts found");
    global_mp_stream->println(
        "   Use 'save' or 'save scriptname' to save scripts");
  } else {
    global_mp_stream->printf(
        "\n   Type 'load <number>' or 'load <name>' to load a script\n\r");
  }

  global_mp_stream->println();
}

void ScriptHistory::findNextScriptNumber() {
  // Scan for existing script_X.py files to find the next available number
  next_script_number = 1;
  for (int i = 1; i <= 100; i++) { // Check up to script_100.py
    String test_script = scripts_dir + "/script_" + String(i) + ".py";
    if (FatFS.exists(test_script)) {
      next_script_number = i + 1; // Set to next available number
    } else {
      break; // Found first gap, use it
    }
  }
}

void ScriptHistory::saveHistoryToFile() {
  String historyPath = scripts_dir + "/history.txt";
  File file = FatFS.open(historyPath, "w");
  if (!file) {
    return; // Fail silently to avoid spam
  }

  for (int i = 0; i < history_count; i++) {
    file.println("===SCRIPT_START===");
    file.print(history[i]);
    file.println("\n===SCRIPT_END===");
  }
  file.close();
}

void ScriptHistory::loadHistoryFromFile() {
  String historyPath = scripts_dir + "/history.txt";
  if (!FatFS.exists(historyPath)) {
    return; // No history file exists yet
  }

  File file = FatFS.open(historyPath, "r");
  if (!file) {
    return; // Fail silently
  }

  String content = file.readString();
  file.close();

  // Parse saved history
  int start = 0;
  while (start < content.length()) {
    int script_start = content.indexOf("===SCRIPT_START===", start);
    if (script_start == -1)
      break;

    int script_end = content.indexOf("===SCRIPT_END===", script_start);
    if (script_end == -1)
      break;

    script_start += 18; // Length of "===SCRIPT_START==="
    if (content.charAt(script_start) == '\n')
      script_start++;

    String script = content.substring(script_start, script_end);
    script.trim();

    if (script.length() > 0 && history_count < MAX_HISTORY) {
      history[history_count++] = script;
    }

    start = script_end + 16; // Length of "===SCRIPT_END==="
  }
}

// REPLEditor method implementations
void REPLEditor::getCurrentLine(String &line, int &line_start, int &cursor_in_line) {
  int last_newline = current_input.lastIndexOf('\n');
  if (last_newline >= 0) {
    line = current_input.substring(last_newline + 1);
    line_start = last_newline + 1;
  } else {
    line = current_input;
    line_start = 0;
  }
  cursor_in_line = cursor_pos - line_start;
}

void REPLEditor::moveCursorToColumn(Stream *stream, int column) {
  stream->print("\033[");
  stream->print(column + 1); // Terminal columns are 1-based
  stream->print("G");
  stream->flush();
}

void REPLEditor::clearToEndOfLine(Stream *stream) {
  stream->print("\033[K"); // CSI K - Erase to Right
  stream->flush();
}

void REPLEditor::clearEntireLine(Stream *stream) {
  stream->print("\033[2K"); // CSI 2 K - Erase All
  stream->flush();
}

void REPLEditor::clearScreen(Stream *stream) {
  stream->print("\033[2J"); // CSI 2 J - Erase All
  stream->print("\033[H");  // CSI H - Home cursor
  stream->flush();
}

void REPLEditor::clearBelow(Stream *stream) {
  stream->print("\033[J"); // CSI J - Erase Below
  stream->flush();
}

void REPLEditor::moveCursorUp(Stream *stream, int lines) {
  if (lines > 1) {
    stream->print("\033[");
    stream->print(lines);
    stream->print("A");
  } else {
    stream->print("\033[A");
  }
  stream->flush();
}

void REPLEditor::moveCursorDown(Stream *stream, int lines) {
  if (lines > 1) {
    stream->print("\033[");
    stream->print(lines);
    stream->print("B");
  } else {
    stream->print("\033[B");
  }
  stream->flush();
}

void REPLEditor::redrawCurrentLine(Stream *stream) {
  String current_line;
  int line_start, cursor_in_line;
  getCurrentLine(current_line, line_start, cursor_in_line);

  // Clear entire current line and start fresh
  stream->print("\r");
  clearEntireLine(stream);

  // Draw prompt
  if (in_multiline_mode) {
    changeTerminalColor(replColors[1], true, stream);
    stream->print("... ");
  } else {
    changeTerminalColor(replColors[1], true, stream);
    stream->print(">>> ");
  }

  // Draw line content
  changeTerminalColor(replColors[3], false, stream);
  stream->print(current_line);

  // Position cursor correctly
  int prompt_length = 4; // ">>> " or "... " both are 4 chars
  moveCursorToColumn(stream, prompt_length + cursor_in_line);
}

void REPLEditor::navigateToLine(Stream *stream, int target_line) {
  // Split input into lines
  String lines = current_input;
  int line_count = 1;
  for (int i = 0; i < lines.length(); i++) {
    if (lines.charAt(i) == '\n')
      line_count++;
  }

  // Find current line number
  int current_line_num = 1;
  for (int i = 0; i < cursor_pos; i++) {
    if (current_input.charAt(i) == '\n')
      current_line_num++;
  }

  if (target_line < 1 || target_line > line_count)
    return;

  int line_diff = target_line - current_line_num;
  if (line_diff > 0) {
    moveCursorDown(stream, line_diff);
  } else if (line_diff < 0) {
    moveCursorUp(stream, -line_diff);
  }
}

void REPLEditor::backspaceOverNewline(Stream *stream) {
  if (cursor_pos > 0 && current_input.charAt(cursor_pos - 1) == '\n') {
    // Remove the newline
    current_input.remove(cursor_pos - 1, 1);
    cursor_pos--;

    // Check if we're leaving multiline mode
    if (current_input.indexOf('\n') == -1) {
      in_multiline_mode = false;
    }

    // Move cursor up one line
    moveCursorUp(stream);

    // Find the end of the previous line
    String current_line;
    int line_start, cursor_in_line;
    getCurrentLine(current_line, line_start, cursor_in_line);

    // Move to end of previous line
    int prompt_length = in_multiline_mode ? 4 : 4;
    moveCursorToColumn(stream, prompt_length + current_line.length());
  }
}

void REPLEditor::navigateOverNewline(Stream *stream) {
  if (cursor_pos > 0 && current_input.charAt(cursor_pos - 1) == '\n') {
    // Move cursor position to before the newline
    cursor_pos--;

    // Move cursor up one line visually
    moveCursorUp(stream);

    // Find the end of the previous line
    String current_line;
    int line_start, cursor_in_line;
    getCurrentLine(current_line, line_start, cursor_in_line);

    // Move to end of previous line
    int prompt_length = in_multiline_mode ? 4 : 4;
    moveCursorToColumn(stream, prompt_length + current_line.length());
  }
}

void REPLEditor::loadFromHistory(Stream *stream, const String &historical_input) {
  if (!in_history_mode) {
    original_input = current_input; // Save current input
    in_history_mode = true;
  }

  current_input = historical_input;
  cursor_pos = current_input.length();
  in_multiline_mode = (current_input.indexOf('\n') >= 0);
  escape_state = 0; // Reset escape state when loading new input

  // Redraw the entire input
  redrawFullInput(stream);

  // Small delay to prevent input processing issues
  delayMicroseconds(100);
}

void REPLEditor::exitHistoryMode(Stream *stream) {
  if (in_history_mode) {
    current_input = original_input;
    cursor_pos = current_input.length();
    in_multiline_mode = (current_input.indexOf('\n') >= 0);
    in_history_mode = false;
    escape_state = 0; // Reset escape state
    redrawFullInput(stream);
  }
}

void REPLEditor::redrawFullInput(Stream *stream) {
  // Clear any previously displayed lines by moving up and clearing
  if (last_displayed_lines > 0) {
    // Move cursor up to the beginning of the first displayed line
    moveCursorUp(stream, last_displayed_lines);
  }

  // Move to beginning of current line and clear everything below
  stream->print("\r"); // Go to start of current line
  clearBelow(stream);  // Clear everything below cursor

  // If we have no input, just show prompt
  if (current_input.length() == 0) {
    changeTerminalColor(replColors[1], true, stream);
    stream->print(">>> ");
    stream->flush();
    last_displayed_lines = 0;
    return;
  }

  // Count total lines in new input
  int new_line_count = 0;
  for (int i = 0; i < current_input.length(); i++) {
    if (current_input.charAt(i) == '\n') {
      new_line_count++;
    }
  }

  // Split input into lines for display
  String lines = current_input;
  int line_start = 0;
  int current_line_num = 0;
  int lines_printed = 0;

  // Display each line with proper prompt
  for (int i = 0; i <= lines.length(); i++) {
    if (i == lines.length() || lines.charAt(i) == '\n') {
      String line = lines.substring(line_start, i);

      // Show appropriate prompt
      if (current_line_num == 0) {
        changeTerminalColor(replColors[1], true, stream);
        stream->print(">>> ");
      } else {
        changeTerminalColor(replColors[1], true, stream);
        stream->print("... ");
      }

      // Show line content
      changeTerminalColor(replColors[3], false, stream);
      stream->print(line);
      clearToEndOfLine(stream); // Clear any trailing characters

      // Add newline if not the last line
      if (i < lines.length()) {
        stream->println();
        lines_printed++;
      }

      line_start = i + 1;
      current_line_num++;
    }
  }

  // Update tracking for next redraw
  last_displayed_lines = new_line_count;

  // Position cursor at the end of input
  cursor_pos = current_input.length();
  stream->flush();
}

void REPLEditor::reset() {
  current_input = "";
  cursor_pos = 0;
  in_multiline_mode = false;
  first_run = true;
  escape_state = 0;
  original_input = "";
  in_history_mode = false;
  // Don't reset multiline mode settings - preserve user's choice
  // multiline_override, multiline_forced_on, multiline_forced_off should persist
  last_displayed_lines = 0;
}

void REPLEditor::fullReset() {
  current_input = "";
  cursor_pos = 0;
  in_multiline_mode = false;
  first_run = true;
  escape_state = 0;
  original_input = "";
  in_history_mode = false;
  multiline_override = false;
  multiline_forced_on = false;
  multiline_forced_off = false;
  last_displayed_lines = 0;
}

// Note: enterPasteMode function removed - replaced with "new" command that opens eKilo editor
// for creating new scripts. This provides a better user experience than paste mode.

// New functions for single command execution from main.cpp


char result_buffer[64];

void getMicroPythonCommandFromStream(Stream *stream) {
  String command = "";
  while (stream->available()) {
    command += (char)stream->read();
  }
  command.trim();
  
  if (command.length() > 0) {
    bool success = executeSinglePythonCommandFormatted(command.c_str(), result_buffer, sizeof(result_buffer));
    stream->printf(result_buffer);
  }
}

/**
 * Initialize MicroPython quietly without any output
 * Returns true if successful, false if failed
 */
bool initMicroPythonQuiet(void) {
  if (mp_initialized) {
    return true;
  }

  // Store original stream and redirect to null
  Stream *original_stream = global_mp_stream;
  global_mp_stream = nullptr;
  global_mp_stream_ptr = nullptr;

  // Get proper stack pointer
  char stack_dummy;
  char *stack_top = &stack_dummy;

  // Initialize MicroPython silently
  mp_embed_init(mp_heap, sizeof(mp_heap), stack_top);
  mp_initialized = true;
  mp_repl_active = false;

  // Restore original stream
  global_mp_stream = original_stream;
  global_mp_stream_ptr = (void *)original_stream;
  
  // Import all jumperless functions and constants globally (silently)
  // This ensures everything is available for single commands without prefix
  mp_embed_exec_str(
      "try:\n"
      "    import jumperless\n"
      "    from jumperless import *\n"
      "    globals()['jumperless'] = jumperless\n"
      "except: pass\n");
  
  return true;
}

// Function output type enumeration
enum FunctionOutputType {
  OUTPUT_NONE,           // No output formatting
  OUTPUT_VOLTAGE,        // Format as voltage with V unit
  OUTPUT_CURRENT,        // Format as current with mA unit  
  OUTPUT_POWER,          // Format as power with mW unit
  OUTPUT_GPIO_STATE,     // Format as HIGH/LOW
  OUTPUT_GPIO_DIR,       // Format as INPUT/OUTPUT
  OUTPUT_GPIO_PULL,      // Format as PULLUP/NONE/PULLDOWN
  OUTPUT_BOOL_CONNECTED, // Format as CONNECTED/DISCONNECTED
  OUTPUT_BOOL_YESNO,     // Format as YES/NO
  OUTPUT_COUNT,          // Format as simple number
  OUTPUT_FLOAT           // Format as float with precision
};

// Function type mapping structure
struct FunctionTypeMap {
  const char* function_name;
  FunctionOutputType output_type;
};

/**
 * Global mapping of function names to their output types for formatted printing
 */
static const FunctionTypeMap function_type_map[] = {
  // DAC functions
  {"dac_set", OUTPUT_NONE},
  {"dac_get", OUTPUT_VOLTAGE},
  
  // ADC functions  
  {"adc_get", OUTPUT_VOLTAGE},
  
  // INA functions
  {"ina_get_current", OUTPUT_CURRENT},
  {"ina_get_voltage", OUTPUT_VOLTAGE},
  {"ina_get_bus_voltage", OUTPUT_VOLTAGE},
  {"ina_get_power", OUTPUT_POWER},
  
  // GPIO functions
  {"gpio_set", OUTPUT_NONE},
  {"gpio_get", OUTPUT_GPIO_STATE},
  {"gpio_set_dir", OUTPUT_NONE},
  {"gpio_get_dir", OUTPUT_GPIO_DIR},
  {"gpio_set_pull", OUTPUT_NONE},
  {"gpio_get_pull", OUTPUT_GPIO_PULL},
  
  // Node functions
  {"connect", OUTPUT_BOOL_CONNECTED},
  {"disconnect", OUTPUT_NONE},
  {"nodes_clear", OUTPUT_NONE},
  {"is_connected", OUTPUT_BOOL_CONNECTED},
  
  // OLED functions
  {"oled_print", OUTPUT_NONE},
  {"oled_clear", OUTPUT_NONE},
  {"oled_show", OUTPUT_NONE},
  {"oled_connect", OUTPUT_BOOL_YESNO},
  {"oled_disconnect", OUTPUT_NONE},
  
  // Other functions
  {"arduino_reset", OUTPUT_NONE},
  {"probe_tap", OUTPUT_NONE},
  {"clickwheel_up", OUTPUT_NONE},
  {"clickwheel_down", OUTPUT_NONE},
  {"clickwheel_press", OUTPUT_NONE},
  {"run_app", OUTPUT_NONE},
  {"help", OUTPUT_NONE},
  
  {nullptr, OUTPUT_NONE} // End marker
};

/**
 * List of jumperless module function names for automatic prefix detection
 */
static const char* jumperless_functions[] = {
  // DAC functions
  "dac_set", "dac_get",
  // ADC functions  
  "adc_get",
  // INA functions
  "ina_get_current", "ina_get_voltage", "ina_get_bus_voltage", "ina_get_power",
  // GPIO functions
  "gpio_set", "gpio_get", "gpio_set_dir", "gpio_get_dir", "gpio_set_pull", "gpio_get_pull",
  // Node functions
  "connect", "disconnect", "nodes_clear", "is_connected",
  // OLED functions
  "oled_print", "oled_clear", "oled_show", "oled_connect", "oled_disconnect",
  // Other functions
  "arduino_reset", "probe_tap", "clickwheel_up", "clickwheel_down", "clickwheel_press", "run_app", "help",
  nullptr // End marker
};

/**
 * Check if a function name is a jumperless module function
 */
bool isJumperlessFunction(const char* function_name) {
  for (int i = 0; jumperless_functions[i] != nullptr; i++) {
    if (strcmp(function_name, jumperless_functions[i]) == 0) {
      return true;
    }
  }
  return false;
}

/**
 * Get the output type for a function name
 */
FunctionOutputType getFunctionOutputType(const char* function_name) {
  for (int i = 0; function_type_map[i].function_name != nullptr; i++) {
    if (strcmp(function_type_map[i].function_name, function_name) == 0) {
      return function_type_map[i].output_type;
    }
  }
  return OUTPUT_NONE;
}

/**
 * Extract function name from a command string
 * e.g., "gpio_get(2)" -> "gpio_get"
 */
String extractFunctionName(const String& command) {
  int paren_pos = command.indexOf('(');
  if (paren_pos == -1) {
    return command; // No parentheses found
  }
  
  String func_name = command.substring(0, paren_pos);
  func_name.trim();
  
  // Since functions are now globally imported, no prefix handling needed
  return func_name;
}

/**
 * Format a result value based on the function output type
 */
String formatResult(float value, FunctionOutputType output_type) {
  switch (output_type) {
    case OUTPUT_VOLTAGE:
      return String(value, 3) + "V";
      
    case OUTPUT_CURRENT:
      if (value >= 1000.0f) {
        return String(value / 1000.0f, 3) + "A";
      } else {
        return String(value, 1) + "mA";
      }
      
    case OUTPUT_POWER:
      if (value >= 1000.0f) {
        return String(value / 1000.0f, 3) + "W";
      } else {
        return String(value, 1) + "mW";
      }
      
    case OUTPUT_GPIO_STATE:
      return (value != 0.0f) ? "HIGH" : "LOW";
      
    case OUTPUT_GPIO_DIR:
      return (value != 0.0f) ? "OUTPUT" : "INPUT";
      
    case OUTPUT_GPIO_PULL:
      if (value > 0.5f) return "PULLUP";
      else if (value < -0.5f) return "PULLDOWN";
      else return "NONE";
      
    case OUTPUT_BOOL_CONNECTED:
      return (value != 0.0f) ? "CONNECTED" : "DISCONNECTED";
      
    case OUTPUT_BOOL_YESNO:
      return (value != 0.0f) ? "YES" : "NO";
      
    case OUTPUT_COUNT:
      return String((int)value);
      
    case OUTPUT_FLOAT:
      return String(value, 3);
      
    case OUTPUT_NONE:
    default:
      return "OK";
  }
}

/**
 * Parse a command and add jumperless. prefix if needed
 * Returns a new String with the parsed command
 */
String parseCommandWithPrefix(const char* command) {
  // Since all jumperless functions are now globally imported,
  // we no longer need to add prefixes - just return the command as-is
  String cmd = String(command);
  cmd.trim();
  return cmd;
}

/**
 * Execute a single MicroPython command with automatic initialization and prefix handling
 * This function can be called from main.cpp
 * 
 * @param command The command to execute (e.g., "gpio_get(2)" or "dac_set(0, 3.3)")
 * @param result_buffer Optional buffer to store string result (can be nullptr)
 * @param buffer_size Size of result buffer
 * @return true if command executed successfully, false otherwise
 */
bool executeSinglePythonCommand(const char* command, char* result_buffer, size_t buffer_size) {
  // Initialize quietly if needed
  if (!mp_initialized) {
    if (!initMicroPythonQuiet()) {
      if (result_buffer && buffer_size > 0) {
        strncpy(result_buffer, "ERROR: Failed to initialize MicroPython", buffer_size - 1);
        result_buffer[buffer_size - 1] = '\0';
      }
      return false;
    }
  }
  
  // Parse command and add prefix if needed
  String parsed_command = parseCommandWithPrefix(command);
  
  // Clear result buffer
  if (result_buffer && buffer_size > 0) {
    memset(result_buffer, 0, buffer_size);
  }
  
  bool success = true;
  
  // Execute the command directly - MicroPython will handle errors internally
  mp_embed_exec_str(parsed_command.c_str());
  

  if (result_buffer && buffer_size > 0) {
    strncpy(result_buffer, "OK", buffer_size - 1);
    result_buffer[buffer_size - 1] = '\0';
  }
  
  return success;
}

/**
 * Enhanced command execution with formatted output
 * This version captures the return value and formats it according to function type
 */
bool executeSinglePythonCommandFormatted(const char* command, char* result_buffer, size_t buffer_size) {
  // Initialize quietly if needed
  if (!mp_initialized) {
    if (!initMicroPythonQuiet()) {
      if (result_buffer && buffer_size > 0) {
        strncpy(result_buffer, "ERROR: Failed to initialize MicroPython", buffer_size - 1);
        result_buffer[buffer_size - 1] = '\0';
      }
      return false;
    }
  }
  
  // Parse command and add prefix if needed
  String parsed_command = parseCommandWithPrefix(command);
  
  // Clear result buffer
  if (result_buffer && buffer_size > 0) {
    memset(result_buffer, 0, buffer_size);
  }
  
  // Note: Formatted output is now handled natively by the jumperless C module
  // Functions automatically return formatted strings like "HIGH", "3.300V", "123.4mA"
  
  // Simply execute the command - formatting is now handled by the native C module
  mp_embed_exec_str(parsed_command.c_str());
  
  // if (result_buffer && buffer_size > 0) {
  //   strncpy(result_buffer, "Formatted by native module", buffer_size - 1);
  //   result_buffer[buffer_size - 1] = '\0';
  // }
  
  return true;
}

/**
 * Execute a single MicroPython command and return float result
 * Useful for functions that return numeric values like adc_get(), gpio_get(), etc.
 * 
 * @param command The command to execute (e.g., "gpio_get(2)")
 * @param result Pointer to store the numeric result
 * @return true if command executed successfully and result is valid, false otherwise
 */
bool executeSinglePythonCommandFloat(const char* command, float* result) {
  if (!result) return false;
  
  // Initialize quietly if needed
  if (!mp_initialized) {
    if (!initMicroPythonQuiet()) {
      return false;
    }
  }
  
  // Parse command and add prefix if needed
  String parsed_command = parseCommandWithPrefix(command);
  
  bool success = true;
  *result = 0.0f;
  
  // For now, just execute the command directly
  // TODO: Implement proper result capture to get the actual return value
  mp_embed_exec_str(parsed_command.c_str());
  
  return success;
}

/**
 * Simple convenience function for common commands
 * Returns the result as a float (useful for sensor readings)
 */
float quickPythonCommand(const char* command) {
  float result = 0.0f;
  executeSinglePythonCommandFloat(command, &result);
  return result;
}

/**
 * Test function to demonstrate single command execution
 * Can be called from main.cpp to test the functionality
 */
void testSingleCommandExecution(void) {
  if (!global_mp_stream) return;
  
  global_mp_stream->println("\n=== Testing Single Command Execution ===");
  
  // Ensure MicroPython is initialized with jumperless module
  if (!mp_initialized) {
    global_mp_stream->println("Initializing MicroPython quietly...");
    if (!initMicroPythonQuiet()) {
      global_mp_stream->println("ERROR: Failed to initialize MicroPython!");
      return;
    }
    global_mp_stream->println("MicroPython initialized successfully");
  }
  
  // Test 1: Simple command with automatic prefix
  global_mp_stream->println("Test 1: GPIO read with automatic prefix");
  char result_buffer[64];
  bool success = executeSinglePythonCommand("gpio_get(2)", result_buffer, sizeof(result_buffer));
  global_mp_stream->printf("Command: gpio_get(2) -> %s (success: %s)\n", 
                           result_buffer, success ? "true" : "false");
  
  // Test 2: Command that already has prefix (should not add another)
  global_mp_stream->println("\nTest 2: Command with existing prefix");
  success = executeSinglePythonCommand("jumperless.dac_set(0, 2.5)", result_buffer, sizeof(result_buffer));
  global_mp_stream->printf("Command: jumperless.dac_set(0, 2.5) -> %s (success: %s)\n", 
                           result_buffer, success ? "true" : "false");
  
  // Test 3: Python command that should not get prefix
  global_mp_stream->println("\nTest 3: Python command (no prefix)");
  success = executeSinglePythonCommand("print('Hello from Python!')", result_buffer, sizeof(result_buffer));
  global_mp_stream->printf("Command: print('Hello from Python!') -> %s (success: %s)\n", 
                           result_buffer, success ? "true" : "false");
  
  // Test 4: Float result function
  global_mp_stream->println("\nTest 4: Float result function");
  float float_result = 0.0f;
  success = executeSinglePythonCommandFloat("adc_get(0)", &float_result);
  global_mp_stream->printf("Command: adc_get(0) -> %.3f (success: %s)\n", 
                           float_result, success ? "true" : "false");
  
  // Test 5: Quick command function
  global_mp_stream->println("\nTest 5: Quick command function");
  float quick_result = quickPythonCommand("gpio_get(1)");
  global_mp_stream->printf("quickPythonCommand('gpio_get(1)') -> %.3f\n", quick_result);
  
  // Test 6: Command parsing demonstration
  global_mp_stream->println("\nTest 6: Command parsing examples");
  String parsed;
  
  parsed = parseCommandWithPrefix("gpio_get(2)");
  global_mp_stream->println("gpio_get(2) -> " + parsed);
  
  parsed = parseCommandWithPrefix("jumperless.dac_set(0, 3.3)");
  global_mp_stream->println("jumperless.dac_set(0, 3.3) -> " + parsed);
  
  parsed = parseCommandWithPrefix("print('test')");
  global_mp_stream->println("print('test') -> " + parsed);
  
  parsed = parseCommandWithPrefix("connect(1, 5)");
  global_mp_stream->println("connect(1, 5) -> " + parsed);
  
  global_mp_stream->println("\n=== Single Command Test Complete ===\n");
}

/**
 * Test function to demonstrate formatted output
 */
void testFormattedOutput(void) {
  if (!global_mp_stream) return;
  
  global_mp_stream->println("\n=== Testing Formatted Output ===");
  
  // Ensure MicroPython is initialized
  if (!mp_initialized) {
    if (!initMicroPythonQuiet()) {
      global_mp_stream->println("ERROR: Failed to initialize MicroPython!");
      return;
    }
  }
  
  char result_buffer[64];
  bool success;
  
  // Test GPIO state formatting
  global_mp_stream->println("\nGPIO State Formatting:");
  success = executeSinglePythonCommandFormatted("gpio_get(2)", result_buffer, sizeof(result_buffer));
  global_mp_stream->printf("  gpio_get(2) -> %s\n", result_buffer);
  
  // Test voltage formatting
  global_mp_stream->println("\nVoltage Formatting:");
  success = executeSinglePythonCommandFormatted("dac_get(0)", result_buffer, sizeof(result_buffer));
  global_mp_stream->printf("  dac_get(0) -> %s\n", result_buffer);
  
  success = executeSinglePythonCommandFormatted("adc_get(1)", result_buffer, sizeof(result_buffer));
  global_mp_stream->printf("  adc_get(1) -> %s\n", result_buffer);
  
  // Test current formatting
  global_mp_stream->println("\nCurrent/Power Formatting:");
  success = executeSinglePythonCommandFormatted("ina_get_current(0)", result_buffer, sizeof(result_buffer));
  global_mp_stream->printf("  ina_get_current(0) -> %s\n", result_buffer);
  
  success = executeSinglePythonCommandFormatted("ina_get_power(0)", result_buffer, sizeof(result_buffer));
  global_mp_stream->printf("  ina_get_power(0) -> %s\n", result_buffer);
  
  // Test GPIO direction formatting
  global_mp_stream->println("\nGPIO Direction Formatting:");
  success = executeSinglePythonCommandFormatted("gpio_get_dir(3)", result_buffer, sizeof(result_buffer));
  global_mp_stream->printf("  gpio_get_dir(3) -> %s\n", result_buffer);
  
  // Test GPIO pull formatting
  global_mp_stream->println("\nGPIO Pull Formatting:");
  success = executeSinglePythonCommandFormatted("gpio_get_pull(4)", result_buffer, sizeof(result_buffer));
  global_mp_stream->printf("  gpio_get_pull(4) -> %s\n", result_buffer);
  
  // Test connection formatting
  global_mp_stream->println("\nConnection Status Formatting:");
  success = executeSinglePythonCommandFormatted("is_connected(1, 5)", result_buffer, sizeof(result_buffer));
  global_mp_stream->printf("  is_connected(1, 5) -> %s\n", result_buffer);
  
  global_mp_stream->println("\n=== Formatted Output Test Complete ===\n");
}






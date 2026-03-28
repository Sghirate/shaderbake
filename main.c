/** SHADERBAKE
 *
 * Cross compiles a set of input shaders to SPIRV,DXIL,MSL and embeds the
 * resulting blobs in a (C/C++) header. Can be used to embed multi platform
 * shaders, for use with SDL3 GPU, directly into an executable.
 *
 * Input files are expected to be of the format:
 *   [<dir>/]<name>(.vert|.frag|.comp)(.spv|.hlsl)
 * The output array constants will then be of the form:
 *   const uint8_t <name>_(vertex|fragment|compute)_(SPIRV|DXIL|MSL) = {
 *     <hex values...>
 *   };
 * For available command line arguments read the code of args_print_help; Or run
 * `shaderbake --help`
 *
 * Requires SDL3 and SDL_shadercross!
 *
 * Largely based on the `cli.c` implementation in SDL_shadercross, with
 * the addition of (threaded) batch processing.
 * Additionally the output structure and generation in `output_write` is loosely
 * based on `binary_to_compressed_c.cpp` from DearImgui.
 * Licenses below.
 */
// SDL_shadercross License:
/*
  Simple DirectMedia Layer Shader Cross Compiler
  Copyright (C) 2024 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
// DearImgui License:
/*
The MIT License (MIT)

Copyright (c) 2014-2026 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
// ShaderBake License for the remaining code (parsing, orchestration, etc):
/*
The MIT License (MIT)

Copyright (c) 2026-2026 Frederick Parotat

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_log.h>
#include <SDL3_shadercross/SDL_shadercross.h>
#ifdef LEAKCHECK
#include <SDL3/SDL_test_memory.h>
#endif
#include <SDL3/SDL_stdinc.h>

#pragma region Helpers
typedef enum {
  SHADER_FORMAT_NONE,
  SHADER_FORMAT_SPIRV,
  SHADER_FORMAT_HLSL,
  SHADER_FORMAT_DXIL,
  SHADER_FORMAT_MSL,

  SHADER_FORMAT_COUNT,
} ShaderFormat;
static const char *g_shaderFormatNames[] = {
    "None", "SPIRV", "HLSL", "DXIL", "MSL",
};
const char *shader_format_name(ShaderFormat a_format) {
  return a_format >= SHADER_FORMAT_COUNT ? "???"
                                         : g_shaderFormatNames[a_format];
}
const char *path_to_base(const char *a_path) {
  const char *base = SDL_strrchr(a_path, '/');
#ifdef _WIN32
  const char *winBase = SDL_strrchr(a_path, '\\');
  if (winBase > base) {
    base = winBase;
  }
#endif
  return base ? (base + 1) : a_path;
}
bool path_to_identifier(const char *a_path, char *out_buf, size_t *out_size) {
  int j = 0;
  int first = 1;
  const char *base = path_to_base(a_path);
  const size_t len = SDL_utf8strlen(base) + 1;
  if (len > (*out_size)) {
    // Output buffer does not accomodate the file path
    return false;
  }
  while (*base && *base != '.') {
    Uint32 codepoint = SDL_StepUTF8(&base, NULL);
    if (codepoint == ' ') {
      // treat space like other separators
      if (!first && j > 0 && out_buf[j - 1] != '_') {
        out_buf[j++] = '_';
      }
    } else if (SDL_isalnum(codepoint)) {
      char c = SDL_tolower((char)codepoint);
      if (first) {
        c = SDL_isalpha(c) ? c : '_';
      }
      out_buf[j++] = c;
      first = 0;
    } else if (codepoint == '_' || codepoint == '-' || codepoint == '.') {
      if (!first && j > 0 && out_buf[j - 1] != '_')
        out_buf[j++] = '_';
    }
  }
  while (j > 0 && out_buf[j - 1] == '_') {
    --j;
  }
  if (j == 0) {
    return false;
  } else {
    out_buf[j] = '\0';
  }
  *out_size = j;
  return true;
}
typedef struct {
  const char *str;
  size_t len;
} StringSpan;
typedef struct {
  char *name;
  const char *path;
  SDL_ShaderCross_ShaderStage stage;
  ShaderFormat input;
  ShaderFormat output;
  uint8_t *data;
  size_t dataSize;
} Job;
static const char *g_stageNames[] = {
    "vertex",
    "fragment",
    "compute",
};
static struct {
  const char *ext;
  SDL_ShaderCross_ShaderStage stage;
  ShaderFormat format;
} g_inputFileTypes[] = {
    {
        .ext = ".vert.spv",
        .stage = SDL_SHADERCROSS_SHADERSTAGE_VERTEX,
        .format = SHADER_FORMAT_SPIRV,
    },
    {
        .ext = ".frag.spv",
        .stage = SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT,
        .format = SHADER_FORMAT_SPIRV,
    },
    {
        .ext = ".comp.spv",
        .stage = SDL_SHADERCROSS_SHADERSTAGE_COMPUTE,
        .format = SHADER_FORMAT_SPIRV,
    },
    {
        .ext = ".vert.hlsl",
        .stage = SDL_SHADERCROSS_SHADERSTAGE_VERTEX,
        .format = SHADER_FORMAT_HLSL,
    },
    {
        .ext = ".frag.hlsl",
        .stage = SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT,
        .format = SHADER_FORMAT_HLSL,
    },
    {
        .ext = ".comp.hlsl",
        .stage = SDL_SHADERCROSS_SHADERSTAGE_COMPUTE,
        .format = SHADER_FORMAT_HLSL,
    },
};
#pragma endregion Helpers

#pragma region Logger
SDL_LogCategory log_category = SDL_LOG_CATEGORY_APPLICATION;
const char *log_name = "shaderbake";
void log_msg(SDL_LogPriority prio, const char *fmt, va_list ap) {
  if (!log_name) {
    SDL_LogMessageV(log_category, prio, fmt, ap);
    return;
  }

  va_list ap_copy;
  va_copy(ap_copy, ap);
  int msg_len = SDL_vsnprintf(NULL, 0, fmt, ap_copy);
  va_end(ap_copy);

  int prefix_len = SDL_strlen(log_name) + 3; /* "[name] " */

  char *buf = (char *)SDL_malloc(prefix_len + msg_len + 1);
  if (!buf)
    return;

  /* Write prefix */
  SDL_snprintf(buf, prefix_len + 1, "[%s] ", log_name);

  /* Write formatted message */
  SDL_vsnprintf(buf + prefix_len, msg_len + 1, fmt, ap);

  SDL_LogMessage(log_category, prio, "%s", buf);

  SDL_free(buf);
}
void log_err(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  log_msg(SDL_LOG_PRIORITY_ERROR, fmt, ap);
  va_end(ap);
}
void log_warn(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  log_msg(SDL_LOG_PRIORITY_WARN, fmt, ap);
  va_end(ap);
}
void log_info(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  log_msg(SDL_LOG_PRIORITY_INFO, fmt, ap);
  va_end(ap);
}
#pragma endregion Logger

#pragma region Args
/** Program arguments. */
struct Args {
  char **files;
  unsigned int numFiles;
  Job *jobs;
  unsigned int numJobs;
  SDL_ShaderCross_HLSL_Define *defines;
  unsigned int numDefines;
  unsigned int numWorkers;
  char *output;
  char *includeDir;
  char *mslVersion;
  bool isPsslCompatRequested;
  bool isHelpRequested;
  bool isDebugRequested;
};
typedef struct Args Args;
bool args_parse_size(unsigned int *out_value, const char *a_name,
                     char *a_input) {
  if (!a_input) {
    log_err("[Arg:%s] Invalid input", a_name);
    return false;
  }

  bool done = false;
  bool firstDigit = true;
  *out_value = 0;
  do {
    char c = *a_input;
    switch (c) {
    case '\0': {
      done = true;
    } break;
    case '\t': // intended fall-through
    case '\n': // intended fall-through
    case '\r': // intended fall-through
    case '\f': // intended fall-through
    case '\v': // intended fall-through
    case ' ': {
      // ignore whitespace
    } break;
    default: {
      if (c >= '0' && c <= '9') {
        if (c == '0' && firstDigit) {
          log_err("[Arg:%s] First digit must not be zero!", a_name);
          return false;
        }
        (*out_value) *= 10;
        (*out_value) += (c - '0');
        firstDigit = false;
      } else {
        log_err("[Arg:%s] Unexpected character in number: %c", a_name, c);
        return false;
      }
    }
    }
    ++a_input;
  } while (!done);
  return true;
}
bool args_parse(Args *out_args, int a_argc, char *a_argv[]) {
  bool success = true;
  for (int i = 1; i < a_argc; ++i) {
    if (SDL_strcmp("-h", a_argv[i]) == 0 ||
        SDL_strcmp("--help", a_argv[i]) == 0) {
      out_args->isHelpRequested = true;
    } else if (SDL_strcmp("-g", a_argv[i]) == 0 ||
               SDL_strcmp("--debug", a_argv[i]) == 0) {
      out_args->isDebugRequested = true;
    } else if (SDL_strcmp("-p", a_argv[i]) == 0 ||
               SDL_strcmp("--pssl", a_argv[i]) == 0) {
      out_args->isPsslCompatRequested = true;
    } else if (SDL_strcmp("-o", a_argv[i]) == 0 ||
               SDL_strcmp("--output", a_argv[i]) == 0) {
      if (out_args->output) {
        log_err("Output can only be set once");
        success = false;
      } else if (i + 1 >= a_argc) {
        log_err("%s requires an argument", a_argv[i]);
        success = false;
      } else {
#ifdef SHADERBAKE_COPY_ARG_STRINGS
        const size_t len = SDL_utf8strlen(a_argv[i + 1]) + 1;
        out_args->output = SDL_malloc(len);
        SDL_utf8strlcpy(out_args->output, (const char *)a_argv[i + 1], len);
#else
        out_args->output = a_argv[i + 1];
#endif
      }
      ++i;
    } else if (SDL_strcmp("--msl-version", a_argv[i] == 0)) {
      if (i + 1 >= a_argc) {
        log_err("%s requires an argument", a_argv[i]);
        success = false;
      } else {
        out_args->mslVersion = a_argv[i + 1];
      }
      ++i;
    } else if (SDL_strcmp("-w", a_argv[i]) == 0 ||
               SDL_strcmp("--workers", a_argv[i]) == 0) {
      if (out_args->numWorkers != 0) {
        log_err("Workers can only be set once");
        success = false;
      } else if (i + 1 >= a_argc) {
        log_err("%s requires an argument", a_argv[i]);
        success = false;
      } else if (!args_parse_size(&(out_args->numWorkers), a_argv[i],
                                  a_argv[i + 1])) {
        success = false;
      }
      ++i;
    } else if (SDL_strcmp("-I", a_argv[i]) == 0 ||
               SDL_strcmp("--include", a_argv[i]) == 0) {
      if (out_args->includeDir) {
        log_err("Include dir can only be set once");
        success = false;
      } else if (i + 1 >= a_argc) {
        log_err("%s requires an argument", a_argv[i]);
        success = false;
      } else {
#ifdef SHADERBAKE_COPY_ARG_STRINGS
        const size_t len = SDL_utf8strlen(a_argv[i + 1]) + 1;
        out_args->includeDir = SDL_malloc(len);
        SDL_utf8strlcpy(out_args->includeDir, (const char *)a_argv[i + 1], len);
#else
        out_args->includeDir = a_argv[i + 1];
#endif
      }
      ++i;
    } else if (SDL_strncmp(a_argv[i], "-D", SDL_strlen("-D")) == 0) {
      ++out_args->numDefines;
      out_args->defines =
          SDL_realloc(out_args->defines, sizeof(SDL_ShaderCross_HLSL_Define) *
                                             out_args->numDefines);
      char *equalSign = SDL_strchr(a_argv[i], '=');
      if (equalSign) {
        out_args->defines[out_args->numDefines - 1].value = equalSign + 1;
        size_t len =
            out_args->defines[out_args->numDefines - 1].value - a_argv[i] - 2;
        out_args->defines[out_args->numDefines - 1].name = SDL_malloc(len);
        SDL_utf8strlcpy(out_args->defines[out_args->numDefines - 1].name,
                        (const char *)a_argv[i] + 2, len);
        ;
      } else {
        out_args->defines[out_args->numDefines - 1].value = NULL;
        size_t len = SDL_utf8strlen(a_argv[i]) + 1 - 2;
        out_args->defines[out_args->numDefines - 1].name = SDL_malloc(len);
        SDL_utf8strlcpy(out_args->defines[out_args->numDefines - 1].name,
                        (const char *)a_argv[i] + 2, len);
      }
    } else {
      ++out_args->numFiles;
      out_args->files = SDL_realloc(out_args->files,
                                    sizeof(const char *) * out_args->numFiles);
#ifdef SHADERBAKE_COPY_ARG_STRINGS
      const size_t len = SDL_utf8strlen(a_argv[i]) + 1;
      out_args->files[out_args->numFiles - 1] = SDL_malloc(len);
      SDL_utf8strlcpy(out_args->files[out_args->numFiles - 1], a_argv[i], len);
#else
      out_args->files[out_args->numFiles - 1] = a_argv[i];
#endif
    }
  }
  if (!out_args->isHelpRequested && !out_args->output) {
    success = false;
    log_err("No output provided.");
  }
  if (!out_args->isHelpRequested && !out_args->files) {
    log_warn("No input files provided. Nothing to do.");
  }

  // null-terminate the defines array
  if (out_args->defines != NULL) {
    out_args->defines =
        SDL_realloc(out_args->defines, sizeof(SDL_ShaderCross_HLSL_Define) *
                                           (out_args->numDefines + 1));
    out_args->defines[out_args->numDefines].name = NULL;
    out_args->defines[out_args->numDefines].value = NULL;
  }

  // Build jobs from input files and formats
  if (out_args->numFiles > 0) {
    // TODO: get desired output formats, for now build for all
    const ShaderFormat outputFormats[] = {
        SHADER_FORMAT_SPIRV,
        SHADER_FORMAT_DXIL,
        SHADER_FORMAT_MSL,
    };
    const int numFormats = 3;
    out_args->numJobs = out_args->numFiles * numFormats;
    out_args->jobs = SDL_malloc(sizeof(Job) * out_args->numJobs);
    int j = 0;
    for (unsigned int i = 0; i < out_args->numFiles; ++i) {
      Job job = {
          .path = out_args->files[i],
      };
      char name[128] = {};
      bool found = false;
      const int numTypes =
          sizeof(g_inputFileTypes) / sizeof(g_inputFileTypes[0]);
      for (int t = 0; t < numTypes; ++t) {
        const char *match = SDL_strstr(job.path, g_inputFileTypes[t].ext);
        if (!match) {
          continue;
        }

        size_t nameLen = 128;
        if (!path_to_identifier(job.path, name, &nameLen)) {
          log_err("Could not turn oath into shader name: %s", job.path);
          success = false;
          continue;
        }

        found = true;
        job.stage = g_inputFileTypes[t].stage;
        job.input = g_inputFileTypes[t].format;
        break;
      }
      if (!found) {
        success = false;
        log_err("Could not determine input type or language: %s. "
                "Expected *(.frag|.vert|.comp)(.spv|.hlsl)",
                job.path);
        continue;
      }
      for (int s = 0; s < numFormats; ++s) {
        job.output = outputFormats[s];
        out_args->jobs[j] = job; // copy
        size_t nameLen = SDL_utf8strlen(name) + 1;
        out_args->jobs[j].name = SDL_malloc(nameLen);
        SDL_utf8strlcpy(out_args->jobs[j].name, name, nameLen);
        ++j;
      }
    }
  }

  return success;
}
void args_print_help() {
  int column_width = 32;
  SDL_Log("Usage: shaderbake [options] <input files>\n");
  SDL_Log("\n");
  SDL_Log("Required options:\n");
  SDL_Log("  %-*s %s", column_width, "-o | --output <value>",
          "Output file. Will contain all generated shader blobs.\n");
  SDL_Log("\n");
  SDL_Log("Optional options:\n");
  SDL_Log("  %-*s %s", column_width, "-h | --help",
          "Show this help message and exit.\n");
  SDL_Log("  %-*s %s", column_width, "-g | --debug",
          "Enable debug information in the generated shaders.\n");
  SDL_Log("  %-*s %s", column_width, "-w | --workers <value>",
          "Number of worker threads for parallel compilation. Defaults to\n"
          "                                  logical CPU core count.\n");
  SDL_Log("  %-*s %s", column_width, "-I | --include <directory>",
          "Directory to search for include files.\n");
  SDL_Log("  %-*s %s", column_width, "-D<name>[=<value>]",
          "Define a preprocessor macro. Can be specified multiple times.\n");
  SDL_Log("  %-*s %s", column_width, "--msl-version <value>",
          "Target MSL version. Only used when transpiling to MSL. The default "
          "is 1.2.0.");
  SDL_Log(
      "  %-*s %s", column_width, "-p | --pssl",
      "Generate PSSL-compatible shader. Destination format should be HLSL.");
  SDL_Log("\n");
  SDL_Log("Input files should be of the format: "
          "[<dir>/]<name>(.vert|.frag|.comp)(.spv|.hlsl)\n");
}
void args_free(Args *a_args) {
  if (!a_args) {
    return;
  }

#ifdef SHADERBAKE_COPY_ARG_STRINGS
  for (int i = 0; i < a_args->numFiles; ++i) {
    SDL_free(a_args->files[i]);
  }
#endif
  SDL_free(a_args->files);
  a_args->files = NULL;
  a_args->numFiles = 0;

  for (unsigned int i = 0; i < a_args->numJobs; ++i) {
    Job *job = &a_args->jobs[i];
    SDL_free(job->name);
    job->name = NULL;
    SDL_free(job->data);
    job->data = NULL;
    job->dataSize = 0;
  }
  SDL_free(a_args->jobs);
  a_args->jobs = NULL;

  for (unsigned int i = 0; i < a_args->numDefines; ++i) {
    SDL_free(a_args->defines[i].name);
  }
  SDL_free(a_args->defines);
  a_args->defines = NULL;
  a_args->numDefines = 0;

  a_args->numWorkers = 0;

#ifdef SHADERBAKE_COPY_ARG_STRINGS
  SDL_free(a_args->output);
#endif
  a_args->output = NULL;

#ifdef SHADERBAKE_COPY_ARG_STRINGS
  SDL_free(a_args->includeDir);
#endif
  a_args->includeDir = NULL;

  a_args->isDebugRequested = false;

  a_args->isHelpRequested = false;
}
#pragma endregion Args

#pragma region Compiler
typedef struct {
  Args *args;
  SDL_AtomicU32 nextJob;
  bool success;
} CompilerState;
int compiler_worker(void *ptr) {
  CompilerState *state = ptr;

  while (true) {
    const uint32_t idx = SDL_AddAtomicU32(&state->nextJob, 1);
    if (idx >= state->args->numJobs) {
      break;
    }

    Job *job = &state->args->jobs[idx];
    size_t fileSize;
    uint8_t *fileData = SDL_LoadFile(job->path, &fileSize);
    if (!fileData) {
      log_err("Invalid shader file: %s", job->path);
      continue;
    }

    if (job->input == SHADER_FORMAT_SPIRV) {
      SDL_ShaderCross_SPIRV_Info spirvInfo;
      spirvInfo.bytecode = fileData;
      spirvInfo.bytecode_size = fileSize;
      spirvInfo.entrypoint = "main";
      spirvInfo.shader_stage = job->stage;
      spirvInfo.props = SDL_CreateProperties();
      if (state->args->isDebugRequested) {
        SDL_SetBooleanProperty(spirvInfo.props,
                               SDL_SHADERCROSS_PROP_SHADER_DEBUG_ENABLE_BOOLEAN,
                               true);
        SDL_SetStringProperty(spirvInfo.props,
                              SDL_SHADERCROSS_PROP_SHADER_DEBUG_NAME_STRING,
                              job->name);
      }
      if (job->output == SHADER_FORMAT_SPIRV) {
        job->data = fileData;
        job->dataSize = fileSize;
      } else if (job->output == SHADER_FORMAT_HLSL) {
        if (state->args->isPsslCompatRequested) {
          SDL_SetBooleanProperty(
              spirvInfo.props,
              SDL_SHADERCROSS_PROP_SPIRV_PSSL_COMPATIBILITY_BOOLEAN, true);
        }
        job->data = SDL_ShaderCross_TranspileHLSLFromSPIRV(&spirvInfo);
      } else if (job->output == SHADER_FORMAT_DXIL) {
        job->data =
            SDL_ShaderCross_CompileDXILFromSPIRV(&spirvInfo, &job->dataSize);
      } else if (job->output == SHADER_FORMAT_MSL) {
        if (state->args->mslVersion) {
          SDL_SetStringProperty(spirvInfo.props,
                                SDL_SHADERCROSS_PROP_SPIRV_MSL_VERSION_STRING,
                                state->args->mslVersion);
        }
        job->data = SDL_ShaderCross_TranspileMSLFromSPIRV(&spirvInfo);
      } else {
        log_err("Unsupported output format: %s (%u)",
                shader_format_name(job->output), job->output);
      }
      SDL_DestroyProperties(spirvInfo.props);
    } else if (job->input == SHADER_FORMAT_HLSL) {
      SDL_ShaderCross_HLSL_Info hlslInfo;
      hlslInfo.source = SDL_reinterpret_cast(const char *, fileData);
      hlslInfo.entrypoint = "main";
      hlslInfo.include_dir = state->args->includeDir;
      hlslInfo.defines = state->args->defines;
      hlslInfo.shader_stage = job->stage;
      hlslInfo.props = SDL_CreateProperties();
      if (state->args->isDebugRequested) {
        SDL_SetBooleanProperty(hlslInfo.props,
                               SDL_SHADERCROSS_PROP_SHADER_DEBUG_ENABLE_BOOLEAN,
                               true);
        SDL_SetStringProperty(hlslInfo.props,
                              SDL_SHADERCROSS_PROP_SHADER_DEBUG_NAME_STRING,
                              job->name);
      }
      if (job->output == SHADER_FORMAT_SPIRV) {
        job->data =
            SDL_ShaderCross_CompileSPIRVFromHLSL(&hlslInfo, &job->dataSize);
      } else if (job->output == SHADER_FORMAT_HLSL) {
        size_t bytecodeSize;
        void *spirv =
            SDL_ShaderCross_CompileSPIRVFromHLSL(&hlslInfo, &bytecodeSize);
        if (!spirv) {
          log_err("Could not compile SPIRV from HLSL (as intermediate for "
                  "HLSL): %s",
                  SDL_GetError());
        } else {
          SDL_ShaderCross_SPIRV_Info spirvInfo;
          spirvInfo.bytecode = spirv;
          spirvInfo.bytecode_size = bytecodeSize;
          spirvInfo.entrypoint = "main";
          spirvInfo.shader_stage = job->stage;
          spirvInfo.props = SDL_CreateProperties();
          if (state->args->isDebugRequested) {
            SDL_SetBooleanProperty(
                spirvInfo.props,
                SDL_SHADERCROSS_PROP_SHADER_DEBUG_ENABLE_BOOLEAN, true);
            SDL_SetStringProperty(spirvInfo.props,
                                  SDL_SHADERCROSS_PROP_SHADER_DEBUG_NAME_STRING,
                                  job->name);
          }
          if (state->args->isPsslCompatRequested) {
            SDL_SetBooleanProperty(
                spirvInfo.props,
                SDL_SHADERCROSS_PROP_SPIRV_PSSL_COMPATIBILITY_BOOLEAN, true);
          }
          job->data = SDL_ShaderCross_TranspileHLSLFromSPIRV(&spirvInfo);
          SDL_free(spirv);
          SDL_DestroyProperties(spirvInfo.props);
        }
      } else if (job->output == SHADER_FORMAT_DXIL) {
        job->data =
            SDL_ShaderCross_CompileDXILFromHLSL(&hlslInfo, &job->dataSize);
      } else if (job->output == SHADER_FORMAT_MSL) {
        size_t bytecodeSize;
        void *spirv =
            SDL_ShaderCross_CompileSPIRVFromHLSL(&hlslInfo, &bytecodeSize);
        if (!spirv) {
          log_err("Could not compile SPIRV from HLSL (as intermediate for "
                  "HLSL): %s",
                  SDL_GetError());
        } else {
          SDL_ShaderCross_SPIRV_Info spirvInfo;
          spirvInfo.bytecode = spirv;
          spirvInfo.bytecode_size = bytecodeSize;
          spirvInfo.entrypoint = "main";
          spirvInfo.shader_stage = job->stage;
          spirvInfo.props = SDL_CreateProperties();
          if (state->args->isDebugRequested) {
            SDL_SetBooleanProperty(
                spirvInfo.props,
                SDL_SHADERCROSS_PROP_SHADER_DEBUG_ENABLE_BOOLEAN, true);
            SDL_SetStringProperty(spirvInfo.props,
                                  SDL_SHADERCROSS_PROP_SHADER_DEBUG_NAME_STRING,
                                  job->name);
          }
          if (state->args->mslVersion) {
            SDL_SetStringProperty(spirvInfo.props,
                                  SDL_SHADERCROSS_PROP_SPIRV_MSL_VERSION_STRING,
                                  state->args->mslVersion);
          }
          job->data = SDL_ShaderCross_TranspileMSLFromSPIRV(&spirvInfo);
          SDL_free(spirv);
          SDL_DestroyProperties(spirvInfo.props);
        }
        // job->data = SDL_ShaderCross_TranspileMSLFromSPIRV(&spirvInfo);
      } else {
        log_err("Unsupported output format: %s (%u)",
                shader_format_name(job->output), job->output);
      }
      SDL_DestroyProperties(hlslInfo.props);
    } else {
      log_err("Unsupported input format: %s (%u)",
              shader_format_name(job->output), job->output);
    }

    if (job->data != fileData) {
      // Only free fileData if its ownership did not transition to the job.
      // Otherwise it's freed, when the job itself is freed.
      SDL_free(fileData);
    }
    if (job->data && !job->dataSize) {
      job->dataSize =
          SDL_utf8strlen(SDL_reinterpret_cast(const char *, job->data));
    } else if (!job->data) {
      log_err("Compilation failed: %s: %s (%u) => %s (%u)", job->path,
              shader_format_name(job->input), job->input,
              shader_format_name(job->output), job->output);
      state->success = false;
    }
  }
  return 0;
}
/** Batch compiler. Relies on inputs & settings provided via Args, including a
 * prepared set of jobs, that can be run by the compiler. Uses a set of workers
 * which invoke SDL_shadercross per job and store the resulting shader blob in
 * Job::data (&Job::dataSize); Job list is stored in Args::jobs.
 *
 * Will NOT write any output.
 *
 *  If a shader job fails, it will report success = false, but still
 * run the remaining jobs, reporting as many errors as possible.
 */
bool compiler_run(Args *a_args) {
  bool success = true;

  size_t numWorkers = a_args->numWorkers;
  if (numWorkers <= 0) {
    // If no worker count is provided; Use number of logical cores
    numWorkers = SDL_GetNumLogicalCPUCores();
  }
  // Don't spawn more workers than files to process
  numWorkers = SDL_min(numWorkers, a_args->numJobs);

  if (numWorkers == 0) {
    // nothing to do
    return true;
  }

  SDL_ShaderCross_Init();

  CompilerState state = {
      .args = a_args,
      .success = true,
  };

  SDL_Thread **workers = SDL_malloc(sizeof(SDL_Thread *) * numWorkers);
  for (unsigned int i = 0; i < numWorkers; ++i) {
    char threadname[64];
    SDL_snprintf(threadname, sizeof(threadname), "CompilerWorker%d", i);
    workers[i] = SDL_CreateThread(compiler_worker, threadname, &state);
  }

  for (unsigned int i = 0; i < numWorkers; ++i) {
    SDL_WaitThread(workers[i], NULL);
    workers[i] = NULL;
  }

  success = state.success;

  SDL_free(workers);

  SDL_ShaderCross_Quit();

  return success;
}
#pragma endregion Compiler

#pragma region Output
/** Output writer.
 *
 * Implements a 'temp-write-and-replace' flow.
 * The temporary file will be created next to the desired output file; With a
 * randomized suffix.
 *
 * Once all data is written to the output, the finalize call
 * will replace the output file with the written data.
 *
 * If finalize is not called, the destructor of this struct will call close;
 * Thus deleting the temporary file.
 *
 * Note: Temp file might linger if the tool
 * crashes for any reason!
 */
bool output_write(Args *a_args) {
  if (!a_args->output) {
    log_err("Output path not provided");
    return false;
  }

  const size_t MaxPath = 1024;
  char path[MaxPath];
  SDL_PathInfo pathInfo;
  do {
    uint32_t rnd = SDL_rand_bits();
    SDL_snprintf(path, MaxPath, "%s.tmp_%u", a_args->output, rnd);
  } while (SDL_GetPathInfo(path, &pathInfo));

  SDL_IOStream *stream = SDL_IOFromFile(path, "w");
  if (!stream) {
    log_err("Could not open output: %s: %s", path, SDL_GetError());
    return false;
  }

  bool success = true;
  // Header & Includes
  SDL_IOprintf(stream, "// File generated by ShaderBake. Manual changes are "
                       "unlikely to persist!\n");
  SDL_IOprintf(stream, "#ifndef __GENERATED_SHADERS__\n#define "
                       "__GENERATED_SHADERS__\n#include <stdint.h>\n\n");
  for (unsigned int j = 0; j < a_args->numJobs; ++j) {
    Job *job = &a_args->jobs[j];
    if (!job->data) {
      success = false;
      log_err("Job has no output data: %s: %s (%u) => %s (%u)", job->path,
              shader_format_name(job->input), job->input,
              shader_format_name(job->output), job->output);
      continue;
    }
    const char *stageName = g_stageNames[job->stage];
    SDL_IOprintf(stream, "const uint8_t %s_%s_%s[%zd] = {", job->name,
                 stageName, shader_format_name(job->output), job->dataSize);
    int column = 0;
    for (unsigned int i = 0; i < job->dataSize; ++i) {
      unsigned char d = *(unsigned char *)(job->data + i);
      if (column == 0) {
        SDL_IOprintf(stream, "\n    ");
      }
      column += SDL_IOprintf(stream, "%d,", d);
      if (column >= 180) {
        column = 0;
      }
    }
    SDL_IOprintf(stream, "\n};\n");
  }
  // Footer
  SDL_IOprintf(stream, "#endif // __GENERATED_SHADERS__\n");
  // Close
  SDL_FlushIO(stream);
  SDL_CloseIO(stream);
  stream = NULL;

  if (success) {
    success = SDL_RenamePath(path, a_args->output);
    if (!success) {
      log_err("Could not replace output: %s => %s: %s", path, a_args->output,
              SDL_GetError());
    }
  }
  if (!success) {
    if (!SDL_RemovePath(path)) {
      log_err("Could not remove temporary file: %s", path);
    }
  }

  return success;
}
#pragma endregion Output

#pragma region Entry Point
int main(int a_argc, char *a_argv[]) {
  Args args = {};

#ifdef LEAKCHECK
  SDLTest_TrackAllocations();
#endif

  if (!args_parse(&args, a_argc, a_argv)) {
    args_print_help();
    args_free(&args);
    return 1;
  }
  if (args.isHelpRequested) {
    args_print_help();
    args_free(&args);
    return 0;
  }

  bool success = compiler_run(&args) && output_write(&args);

  args_free(&args);

#ifdef LEAKCHECK
  SDLTest_LogAllocations();
#endif

  return success ? 0 : 1;
}
#pragma endregion Entry Point
